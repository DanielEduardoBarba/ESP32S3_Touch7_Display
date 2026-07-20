#include "fw_update.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "comm_protocol.h"
#include "app_config.h"

namespace fw_update {
namespace {

const char *TAG = "fw_update";

// --- TYPE_UPDATE commands ---------------------------------------------------
constexpr uint8_t CMD_BEGIN  = 0x01; // payload: size(4BE) crc16(2BE)
constexpr uint8_t CMD_DATA   = 0x02; // payload: offset(4BE) data...
constexpr uint8_t CMD_END    = 0x03; // payload: none
constexpr uint8_t CMD_ACK    = 0x10; // payload: status(1) [0=ok]
constexpr uint8_t CMD_RESULT = 0x11; // payload: status(1) [0=ok]
constexpr uint8_t CMD_VERSION_REQ  = 0x20; // payload: sender's APP_VERSION string
constexpr uint8_t CMD_VERSION_RESP = 0x21; // payload: responder's APP_VERSION string
constexpr uint8_t CMD_PULL_REQ     = 0x22; // payload: none ("send me YOUR image")

constexpr size_t CHUNK_SIZE = APP_UPDATE_CHUNK_SIZE; // data bytes per CMD_DATA frame
constexpr TickType_t ACK_TIMEOUT = pdMS_TO_TICKS(APP_UPDATE_ACK_TIMEOUT_MS);
constexpr int MAX_RETRIES = APP_UPDATE_MAX_RETRIES;

Status s_status;
std::vector<StatusCallback> s_status_cbs;
int64_t s_xfer_start_us = 0;   // for the average-speed estimate

PeerInfo s_peer;
std::vector<PeerInfoCallback> s_peer_cbs;
esp_timer_handle_t s_sync_timer = nullptr;
volatile bool s_sync_responded = false;

// Stop-and-wait handshake: the sender task blocks here until the RX path
// (comm_protocol callback) signals that the peer acknowledged.
SemaphoreHandle_t s_ack_sem = nullptr;
volatile uint8_t s_last_ack_status = 0;
volatile bool s_result_received = false;
volatile uint8_t s_last_result_status = 0;

// Receiver-side transfer context.
esp_ota_handle_t s_rx_ota = 0;
const esp_partition_t *s_rx_partition = nullptr;
uint32_t s_rx_expected_size = 0;
uint16_t s_rx_expected_crc = 0;
uint16_t s_rx_running_crc = 0xFFFF;
uint32_t s_rx_received = 0;
bool s_rx_done_ok = false;         // last transfer ended verified+armed
esp_timer_handle_t s_rx_stall_timer = nullptr;

void setStatus(State state, uint32_t total, uint32_t done, const std::string &message)
{
    // Speed estimate: average bytes/sec since the transfer started. Reset
    // the clock whenever a transfer (re)starts at offset 0.
    const int64_t now_us = esp_timer_get_time();
    const bool transferring = (state == State::Sending || state == State::Receiving);
    if (transferring && done == 0) {
        s_xfer_start_us = now_us;
        s_status.bytes_per_sec = 0;
        s_status.elapsed_ms = 0;
    } else if (transferring && now_us > s_xfer_start_us && s_xfer_start_us > 0) {
        s_status.bytes_per_sec =
            (uint32_t)(((uint64_t)done * 1000000ULL) / (uint64_t)(now_us - s_xfer_start_us));
        s_status.elapsed_ms = (uint32_t)((now_us - s_xfer_start_us) / 1000);
    }

    s_status.state = state;
    s_status.total_bytes = total;
    s_status.done_bytes = done;
    s_status.message = message;
    ESP_LOGI(TAG, "[%d] %s (%lu/%lu, %.1f%%, %lu B/s)", (int)state, message.c_str(),
             (unsigned long)done, (unsigned long)total,
             total > 0 ? 100.0 * done / total : 0.0,
             (unsigned long)s_status.bytes_per_sec);
    for (const auto &cb : s_status_cbs) {
        cb(s_status);
    }
}

// ---------------------------------------------------------------------------
// Image size: parse the ESP image header + segment table so we transfer the
// actual image, not the whole 3MB partition.
// ---------------------------------------------------------------------------

/** Returns the byte size of the app image in `part`, or 0 on parse failure.
 *  Layout: 24-byte header (magic 0xE9, segment_count at offset 1), then per
 *  segment an 8-byte header + data; then a 1-byte checksum padded so the
 *  file is a multiple of 16; +32 bytes if a SHA256 hash is appended. */
uint32_t imageSize(const esp_partition_t *part)
{
    uint8_t header[24];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK || header[0] != 0xE9) {
        return 0;
    }
    uint8_t segment_count = header[1];
    bool hash_appended = header[23] == 1;

    uint32_t offset = sizeof(header);
    for (uint8_t i = 0; i < segment_count; i++) {
        uint8_t seg[8];
        if (esp_partition_read(part, offset, seg, sizeof(seg)) != ESP_OK) {
            return 0;
        }
        uint32_t data_len;
        std::memcpy(&data_len, seg + 4, 4);
        offset += sizeof(seg) + data_len;
        if (offset > part->size) {
            return 0; // corrupt segment table
        }
    }
    offset = (offset + 1 + 15) & ~15u; // checksum byte + pad to 16
    if (hash_appended) {
        offset += 32;
    }
    return offset <= part->size ? offset : 0;
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

/** Sends one TYPE_UPDATE frame and waits for its ACK, retransmitting up to
 *  MAX_RETRIES times on timeout or NACK (a lost frame OR a lost ACK both
 *  land here -- the receiver treats re-sent duplicates as "ACK again, don't
 *  re-write"). Returns false only after the whole generous retry budget is
 *  exhausted. */
bool sendWithAck(uint8_t cmd, const uint8_t *payload, size_t len, const char *what,
                 uint32_t total, uint32_t done)
{
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        xSemaphoreTake(s_ack_sem, 0); // clear any stale ack
        comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, cmd, payload, len);
        if (xSemaphoreTake(s_ack_sem, ACK_TIMEOUT) == pdTRUE && s_last_ack_status == 0) {
            return true;
        }
        ESP_LOGW(TAG, "%s: no/negative ACK (attempt %d/%d)%s", what, attempt, MAX_RETRIES,
                 attempt < MAX_RETRIES ? " -- retransmitting" : "");
        if (attempt < MAX_RETRIES) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s: no ACK, retry %d/%d...", what, attempt, MAX_RETRIES);
            setStatus(State::Sending, total, done, msg);
        }
    }
    return false;
}

void senderTask(void *)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t size = imageSize(running);
    if (size == 0) {
        setStatus(State::Failed, 0, 0, "Could not parse running image size");
        vTaskDelete(nullptr);
        return;
    }

    // Whole-image CRC16, streamed in chunks (no multi-MB buffer needed).
    uint16_t crc = 0xFFFF;
    std::vector<uint8_t> buf(CHUNK_SIZE);
    for (uint32_t off = 0; off < size; off += CHUNK_SIZE) {
        size_t n = std::min<uint32_t>(CHUNK_SIZE, size - off);
        esp_partition_read(running, off, buf.data(), n);
        crc = comm_protocol::crc16(buf.data(), n, crc);
    }

    setStatus(State::Sending, size, 0, "Sending update: begin handshake");

    // BEGIN {size, crc}
    uint8_t begin_payload[6] = {
        static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
        static_cast<uint8_t>(size >> 8),  static_cast<uint8_t>(size),
        static_cast<uint8_t>(crc >> 8),   static_cast<uint8_t>(crc),
    };
    if (!sendWithAck(CMD_BEGIN, begin_payload, sizeof(begin_payload), "BEGIN handshake", size, 0)) {
        setStatus(State::Failed, size, 0, "Peer did not accept update (no ACK after all retries)");
        vTaskDelete(nullptr);
        return;
    }

    // DATA chunks, each acknowledged (with retransmits) before the next.
    std::vector<uint8_t> frame(4 + CHUNK_SIZE);
    for (uint32_t off = 0; off < size; off += CHUNK_SIZE) {
        size_t n = std::min<uint32_t>(CHUNK_SIZE, size - off);
        frame[0] = static_cast<uint8_t>(off >> 24);
        frame[1] = static_cast<uint8_t>(off >> 16);
        frame[2] = static_cast<uint8_t>(off >> 8);
        frame[3] = static_cast<uint8_t>(off);
        esp_partition_read(running, off, frame.data() + 4, n);

        if (!sendWithAck(CMD_DATA, frame.data(), 4 + n, "Chunk", size, off)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Transfer aborted: chunk at %lu not acknowledged after %d retries",
                     (unsigned long)off, MAX_RETRIES);
            setStatus(State::Failed, size, off, msg);
            vTaskDelete(nullptr);
            return;
        }
        if ((off / CHUNK_SIZE) % 64 == 0) { // progress every ~16KB
            setStatus(State::Sending, size, off + n, "Sending update...");
        }
    }

    // END -> wait for the peer's verify verdict, retransmitting END if the
    // verdict never arrives (the receiver replays its verdict on duplicate
    // ENDs, covering a lost RESULT frame too).
    bool got_result = false;
    for (int attempt = 1; attempt <= MAX_RETRIES && !got_result; attempt++) {
        s_result_received = false;
        comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_END, nullptr, 0);
        for (int waited = 0; waited < 100 && !s_result_received; waited++) {
            vTaskDelay(pdMS_TO_TICKS(100)); // give the peer time to verify+arm
        }
        got_result = s_result_received;
        if (!got_result) {
            ESP_LOGW(TAG, "END: no verify verdict (attempt %d/%d) -- retransmitting", attempt, MAX_RETRIES);
        }
    }
    if (got_result && s_last_result_status == 0) {
        setStatus(State::SendDone, size, size, "Update delivered -- peer verified CRC and armed it for boot");
    } else if (!got_result) {
        setStatus(State::Failed, size, size, "No verification verdict from peer after all retries");
    } else {
        setStatus(State::Failed, size, size, "Peer reported verification FAILURE (bad CRC or OTA error)");
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Receiver (runs in comm_protocol's RX context)
// ---------------------------------------------------------------------------

void sendAck(uint8_t status_code)
{
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_ACK, &status_code, 1);
}

// ---------------------------------------------------------------------------
// Peer version sync ("Sync peer device")
// ---------------------------------------------------------------------------

/** Compares two "major.minor.patch" strings; missing parts count as 0. */
PeerCompare compareToOurs(const std::string &peer_version)
{
    int mine[3] = {0, 0, 0}, theirs[3] = {0, 0, 0};
    sscanf(APP_VERSION, "%d.%d.%d", &mine[0], &mine[1], &mine[2]);
    sscanf(peer_version.c_str(), "%d.%d.%d", &theirs[0], &theirs[1], &theirs[2]);
    for (int i = 0; i < 3; i++) {
        if (theirs[i] > mine[i]) {
            return PeerCompare::PeerNewer;
        }
        if (theirs[i] < mine[i]) {
            return PeerCompare::PeerOlder;
        }
    }
    return PeerCompare::Same;
}

void setPeer(const std::string &version)
{
    s_sync_responded = true;
    s_peer.known = true;
    s_peer.no_response = false;
    s_peer.version = version;
    s_peer.compare = compareToOurs(version);
    const char *verdict =
        s_peer.compare == PeerCompare::PeerNewer ? "peer is NEWER -- we are out of date"
        : s_peer.compare == PeerCompare::PeerOlder ? "peer is OLDER -- we are ahead"
                                                    : "same version";
    ESP_LOGI(TAG, "Peer version sync: ours=v%s peer=v%s (%s)", APP_VERSION,
             version.c_str(), verdict);
    for (const auto &cb : s_peer_cbs) {
        cb(s_peer);
    }
}

void handleVersionReq(const uint8_t *payload, size_t len)
{
    setPeer(std::string(reinterpret_cast<const char *>(payload), len));
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_VERSION_RESP,
                             reinterpret_cast<const uint8_t *>(APP_VERSION),
                             std::strlen(APP_VERSION));
}

void handlePullReq()
{
    ESP_LOGI(TAG, "Peer requested to PULL our image -- starting send");
    if (!startSend()) {
        ESP_LOGW(TAG, "Pull request refused: transfer busy or update pending reboot");
    }
}

/** Fires APP_UPDATE_SYNC_TIMEOUT_MS after syncPeer() if no version reply
 *  arrived: the peer likely runs an older firmware that doesn't know the
 *  version-sync command (it was added in v0.0.3). The GUIs then keep
 *  "push" available but disable "pull" (nothing verified to pull). */
void syncTimeoutCb(void *)
{
    if (s_sync_responded) {
        return;
    }
    s_peer.known = false;
    s_peer.no_response = true;
    s_peer.version.clear();
    s_peer.compare = PeerCompare::Unknown;
    ESP_LOGW(TAG,
             "Version sync: no reply within %d ms -- peer is offline or runs an older "
             "firmware without version sync. Push remains possible; pull is disabled.",
             APP_UPDATE_SYNC_TIMEOUT_MS);
    for (const auto &cb : s_peer_cbs) {
        cb(s_peer);
    }
}

void abortReceive(const std::string &why)
{
    if (s_rx_stall_timer != nullptr) {
        esp_timer_stop(s_rx_stall_timer);
    }
    if (s_rx_ota != 0) {
        esp_ota_abort(s_rx_ota);
        s_rx_ota = 0;
    }
    setStatus(State::Failed, s_rx_expected_size, s_rx_received, why);
    sendAck(1);
}

/** Re-arms the receiver's stall watchdog: if the sender goes silent for
 *  longer than its ENTIRE retry budget, clean up the half-written slot
 *  instead of holding it hostage forever. */
void kickRxStallTimer()
{
    if (s_rx_stall_timer != nullptr) {
        esp_timer_stop(s_rx_stall_timer);
        esp_timer_start_once(s_rx_stall_timer, (uint64_t)APP_UPDATE_RX_STALL_MS * 1000);
    }
}

void rxStallTimerCb(void *)
{
    if (s_status.state == State::Receiving) {
        ESP_LOGW(TAG, "Receive stalled: no frame for %d ms -- aborting", APP_UPDATE_RX_STALL_MS);
        abortReceive("Receive stalled (sender went silent) -- aborted");
    }
}

void handleBegin(const uint8_t *payload, size_t len)
{
    if (len != 6) {
        sendAck(1);
        return;
    }
    if (s_status.state == State::Sending || s_status.state == State::ReceiveDone) {
        ESP_LOGW(TAG, "BEGIN rejected: busy or update already pending reboot");
        sendAck(1);
        return;
    }

    uint32_t size = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
                    (uint32_t)payload[2] << 8 | payload[3];
    uint16_t crc = (uint16_t)payload[4] << 8 | payload[5];

    if (s_rx_ota != 0) {
        if (size == s_rx_expected_size && crc == s_rx_expected_crc && s_rx_received == 0) {
            // Duplicate BEGIN: our ACK was lost and the sender retried.
            // The transfer is already set up -- just ACK again.
            ESP_LOGI(TAG, "Duplicate BEGIN (lost ACK) -- re-acknowledging");
            kickRxStallTimer();
            sendAck(0);
            return;
        }
        // A different transfer was half-done; drop it and start fresh.
        ESP_LOGW(TAG, "New BEGIN while a transfer was in flight -- restarting receive");
        esp_ota_abort(s_rx_ota);
        s_rx_ota = 0;
    }

    s_rx_expected_size = size;
    s_rx_expected_crc = crc;
    s_rx_running_crc = 0xFFFF;
    s_rx_received = 0;
    s_rx_done_ok = false;

    s_rx_partition = esp_ota_get_next_update_partition(nullptr);
    if (s_rx_partition == nullptr || s_rx_expected_size > s_rx_partition->size) {
        abortReceive("No usable OTA slot for incoming update");
        return;
    }
    if (esp_ota_begin(s_rx_partition, s_rx_expected_size, &s_rx_ota) != ESP_OK) {
        abortReceive("esp_ota_begin failed");
        return;
    }
    setStatus(State::Receiving, s_rx_expected_size, 0,
              std::string("Receiving update into '") + s_rx_partition->label + "'...");
    kickRxStallTimer();
    sendAck(0);
}

void handleData(const uint8_t *payload, size_t len)
{
    if (s_rx_ota == 0 || len < 5) {
        sendAck(1);
        return;
    }
    uint32_t offset = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
                      (uint32_t)payload[2] << 8 | payload[3];
    const uint8_t *data = payload + 4;
    size_t n = len - 4;

    kickRxStallTimer();

    if (offset + n <= s_rx_received) {
        // Duplicate chunk: we already wrote it but our ACK got lost and the
        // sender retransmitted. Don't re-write -- just ACK again.
        ESP_LOGI(TAG, "Duplicate chunk at %lu (lost ACK) -- re-acknowledging",
                 (unsigned long)offset);
        sendAck(0);
        return;
    }
    if (offset != s_rx_received) { // stop-and-wait means strictly in-order
        abortReceive("Out-of-order chunk -- transfer aborted");
        return;
    }
    if (esp_ota_write(s_rx_ota, data, n) != ESP_OK) {
        abortReceive("esp_ota_write failed");
        return;
    }
    s_rx_running_crc = comm_protocol::crc16(data, n, s_rx_running_crc);
    s_rx_received += n;
    if ((s_rx_received / CHUNK_SIZE) % 64 == 0) {
        setStatus(State::Receiving, s_rx_expected_size, s_rx_received, "Receiving update...");
    }
    sendAck(0);
}

void handleEnd()
{
    uint8_t result = 1;
    if (s_rx_ota == 0) {
        // Nothing in flight -- but if the last transfer already finished
        // successfully, this is a duplicate END (our RESULT frame was
        // lost): replay the good verdict instead of reporting failure.
        if (s_rx_done_ok) {
            ESP_LOGI(TAG, "Duplicate END after success (lost RESULT) -- replaying verdict");
            result = 0;
        }
    } else if (s_rx_received != s_rx_expected_size) {
        abortReceive("Size mismatch at end of transfer");
    } else if (s_rx_running_crc != s_rx_expected_crc) {
        abortReceive("Whole-image CRC MISMATCH -- update rejected, not armed");
    } else if (esp_ota_end(s_rx_ota) != ESP_OK) {
        s_rx_ota = 0;
        setStatus(State::Failed, s_rx_expected_size, s_rx_received,
                  "Image structure validation failed (esp_ota_end)");
    } else if (esp_ota_set_boot_partition(s_rx_partition) != ESP_OK) {
        s_rx_ota = 0;
        setStatus(State::Failed, s_rx_expected_size, s_rx_received,
                  "Could not arm new image for boot");
    } else {
        s_rx_ota = 0;
        result = 0;
        s_rx_done_ok = true;
        if (s_rx_stall_timer != nullptr) {
            esp_timer_stop(s_rx_stall_timer);
        }
        setStatus(State::ReceiveDone, s_rx_expected_size, s_rx_received,
                  std::string("Update verified (CRC OK) and armed in '") + s_rx_partition->label +
                  "' -- reboot when ready");
    }
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_RESULT, &result, 1);
}

void onUpdateFrame(uint8_t cmd, const uint8_t *payload, size_t len)
{
    switch (cmd) {
    case CMD_BEGIN:  handleBegin(payload, len); break;
    case CMD_DATA:   handleData(payload, len); break;
    case CMD_END:    handleEnd(); break;
    case CMD_VERSION_REQ:  handleVersionReq(payload, len); break;
    case CMD_VERSION_RESP: setPeer(std::string(reinterpret_cast<const char *>(payload), len)); break;
    case CMD_PULL_REQ:     handlePullReq(); break;
    case CMD_ACK:
        if (len == 1) {
            s_last_ack_status = payload[0];
            xSemaphoreGive(s_ack_sem);
        }
        break;
    case CMD_RESULT:
        if (len == 1) {
            s_last_result_status = payload[0];
            s_result_received = true;
        }
        break;
    default:
        ESP_LOGW(TAG, "Unknown update cmd 0x%02x", cmd);
        break;
    }
}

} // namespace

void init()
{
    s_ack_sem = xSemaphoreCreateBinary();

    const esp_timer_create_args_t stall_args = {
        .callback = rxStallTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "fw_rx_stall",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&stall_args, &s_rx_stall_timer);

    comm_protocol::onUpdateFrame(onUpdateFrame);
}

AppInfo appInfo()
{
    AppInfo info;
    const esp_partition_t *running = esp_ota_get_running_partition();
    info.running_slot = running->label;
    info.image_size = imageSize(running);
    info.app_version = APP_VERSION;

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(running, &desc) == ESP_OK) {
        info.version = desc.version;
        info.idf_version = desc.idf_ver;
        info.compile_time = std::string(desc.date) + " " + desc.time;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        switch (state) {
        case ESP_OTA_IMG_VALID:          info.ota_state = "valid"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: info.ota_state = "pending verify"; break;
        case ESP_OTA_IMG_NEW:            info.ota_state = "new"; break;
        default:                         info.ota_state = "other"; break;
        }
    } else {
        info.ota_state = "untracked (serial flash)";
    }
    return info;
}

Status status()
{
    return s_status;
}

void syncPeer()
{
    ESP_LOGI(TAG, "Version sync: broadcasting our version v%s", APP_VERSION);
    s_sync_responded = false;
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_VERSION_REQ,
                             reinterpret_cast<const uint8_t *>(APP_VERSION),
                             std::strlen(APP_VERSION));
    if (s_sync_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = syncTimeoutCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "fw_sync_to",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &s_sync_timer);
    }
    esp_timer_stop(s_sync_timer); // restart cleanly if a sync is re-requested
    esp_timer_start_once(s_sync_timer, (uint64_t)APP_UPDATE_SYNC_TIMEOUT_MS * 1000);
}

PeerInfo peerInfo()
{
    return s_peer;
}

bool startSend()
{
    if (s_status.state == State::Sending || s_status.state == State::Receiving) {
        return false;
    }
    if (s_status.state == State::ReceiveDone) {
        return false; // this device has a pending update -- reboot first
    }
    if (xTaskCreatePinnedToCore(senderTask, "fw_send", 6144, nullptr, 5, nullptr,
                                tskNO_AFFINITY) != pdPASS) {
        // Task stacks MUST come from internal RAM -- surface the failure
        // instead of silently doing nothing.
        ESP_LOGE(TAG, "Could not create sender task (internal heap free: %u, min ever: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        setStatus(State::Failed, 0, 0, "Could not start sender (out of internal memory)");
        return false;
    }
    return true;
}

bool startPull()
{
    if (s_status.state == State::Sending || s_status.state == State::Receiving ||
        s_status.state == State::ReceiveDone) {
        return false;
    }
    ESP_LOGI(TAG, "Requesting peer to send us ITS image (pull)");
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_PULL_REQ, nullptr, 0);
    return true;
}

void rebootIntoUpdate()
{
    if (s_status.state != State::ReceiveDone) {
        return;
    }
    ESP_LOGI(TAG, "User confirmed reboot into the received update");
    vTaskDelay(pdMS_TO_TICKS(200)); // let logs/GUI flush
    esp_restart();
}

void onStatusChange(StatusCallback cb)
{
    s_status_cbs.push_back(std::move(cb));
}

void onPeerInfoChange(PeerInfoCallback cb)
{
    s_peer_cbs.push_back(std::move(cb));
}

} // namespace fw_update
