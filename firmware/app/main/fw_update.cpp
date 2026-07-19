#include "fw_update.h"

#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "comm_protocol.h"

namespace fw_update {
namespace {

const char *TAG = "fw_update";

// --- TYPE_UPDATE commands ---------------------------------------------------
constexpr uint8_t CMD_BEGIN  = 0x01; // payload: size(4BE) crc16(2BE)
constexpr uint8_t CMD_DATA   = 0x02; // payload: offset(4BE) data...
constexpr uint8_t CMD_END    = 0x03; // payload: none
constexpr uint8_t CMD_ACK    = 0x10; // payload: status(1) [0=ok]
constexpr uint8_t CMD_RESULT = 0x11; // payload: status(1) [0=ok]

constexpr size_t CHUNK_SIZE = 256;         // data bytes per CMD_DATA frame
constexpr TickType_t ACK_TIMEOUT = pdMS_TO_TICKS(3000);

Status s_status;
std::vector<StatusCallback> s_status_cbs;
int64_t s_xfer_start_us = 0;   // for the average-speed estimate

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

void setStatus(State state, uint32_t total, uint32_t done, const std::string &message)
{
    // Speed estimate: average bytes/sec since the transfer started. Reset
    // the clock whenever a transfer (re)starts at offset 0.
    const int64_t now_us = esp_timer_get_time();
    const bool transferring = (state == State::Sending || state == State::Receiving);
    if (transferring && done == 0) {
        s_xfer_start_us = now_us;
        s_status.bytes_per_sec = 0;
    } else if (transferring && now_us > s_xfer_start_us && s_xfer_start_us > 0) {
        s_status.bytes_per_sec =
            (uint32_t)(((uint64_t)done * 1000000ULL) / (uint64_t)(now_us - s_xfer_start_us));
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
    xSemaphoreTake(s_ack_sem, 0); // clear any stale ack
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_BEGIN, begin_payload, sizeof(begin_payload));
    if (xSemaphoreTake(s_ack_sem, ACK_TIMEOUT) != pdTRUE || s_last_ack_status != 0) {
        setStatus(State::Failed, size, 0, "Peer did not accept update (no/negative ACK)");
        vTaskDelete(nullptr);
        return;
    }

    // DATA chunks, each acknowledged before the next is sent.
    std::vector<uint8_t> frame(4 + CHUNK_SIZE);
    for (uint32_t off = 0; off < size; off += CHUNK_SIZE) {
        size_t n = std::min<uint32_t>(CHUNK_SIZE, size - off);
        frame[0] = static_cast<uint8_t>(off >> 24);
        frame[1] = static_cast<uint8_t>(off >> 16);
        frame[2] = static_cast<uint8_t>(off >> 8);
        frame[3] = static_cast<uint8_t>(off);
        esp_partition_read(running, off, frame.data() + 4, n);

        comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_DATA, frame.data(), 4 + n);
        if (xSemaphoreTake(s_ack_sem, ACK_TIMEOUT) != pdTRUE || s_last_ack_status != 0) {
            setStatus(State::Failed, size, off, "Transfer aborted: chunk not acknowledged");
            vTaskDelete(nullptr);
            return;
        }
        if ((off / CHUNK_SIZE) % 64 == 0) { // progress every ~16KB
            setStatus(State::Sending, size, off + n, "Sending update...");
        }
    }

    // END -> wait for the peer's verify verdict.
    s_result_received = false;
    comm_protocol::sendFrame(comm_protocol::TYPE_UPDATE, CMD_END, nullptr, 0);
    for (int waited = 0; waited < 100 && !s_result_received; waited++) {
        vTaskDelay(pdMS_TO_TICKS(100)); // give the peer time to verify+arm
    }
    if (s_result_received && s_last_result_status == 0) {
        setStatus(State::SendDone, size, size, "Update delivered -- peer verified CRC and armed it for boot");
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

void abortReceive(const std::string &why)
{
    if (s_rx_ota != 0) {
        esp_ota_abort(s_rx_ota);
        s_rx_ota = 0;
    }
    setStatus(State::Failed, s_rx_expected_size, s_rx_received, why);
    sendAck(1);
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

    s_rx_expected_size = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
                         (uint32_t)payload[2] << 8 | payload[3];
    s_rx_expected_crc = (uint16_t)payload[4] << 8 | payload[5];
    s_rx_running_crc = 0xFFFF;
    s_rx_received = 0;

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
        // nothing in flight
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
    comm_protocol::onUpdateFrame(onUpdateFrame);
}

AppInfo appInfo()
{
    AppInfo info;
    const esp_partition_t *running = esp_ota_get_running_partition();
    info.running_slot = running->label;
    info.image_size = imageSize(running);

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

bool startSend()
{
    if (s_status.state == State::Sending || s_status.state == State::Receiving) {
        return false;
    }
    if (s_status.state == State::ReceiveDone) {
        return false; // this device has a pending update -- reboot first
    }
    xTaskCreatePinnedToCore(senderTask, "fw_send", 6144, nullptr, 5, nullptr, tskNO_AFFINITY);
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

} // namespace fw_update
