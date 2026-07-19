#include "comm_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "ports.h"

namespace comm_protocol {
namespace {

const char *TAG = "comm";

// Payload ceiling: fits the largest firmware-update chunk plus headroom,
// while bounding the parser's buffer. (len is 16-bit on the wire.)
constexpr size_t MAX_PAYLOAD = 512;

DialCallback s_dial_cb;
TogglesCallback s_toggles_cb;
FrameCallback s_update_cb;

// ---------------------------------------------------------------------------
// CRCs
// ---------------------------------------------------------------------------

uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                                : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

} // namespace

uint16_t crc16(const uint8_t *data, size_t len, uint16_t seed)
{
    // CRC16-CCITT (poly 0x1021). Also usable incrementally by passing the
    // previous result back in as `seed` (fw_update does this for the whole
    // multi-megabyte image without buffering it).
    uint16_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

namespace {

// ---------------------------------------------------------------------------
// Hex logging
// ---------------------------------------------------------------------------

/** Logs a byte run as "0x02 0x01 ..." lines, capped so a 512-byte firmware
 *  chunk doesn't flood the console (the CRC verdict line that follows tells
 *  you whether the rest was intact anyway). */
void logHexBytes(const char *direction, const uint8_t *data, size_t len)
{
    constexpr size_t MAX_SHOWN = 32;
    char line[MAX_SHOWN * 5 + 16];
    size_t shown = len < MAX_SHOWN ? len : MAX_SHOWN;
    size_t pos = 0;
    for (size_t i = 0; i < shown && pos + 6 < sizeof(line); i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "0x%02x ", data[i]);
    }
    if (len > shown) {
        snprintf(line + pos, sizeof(line) - pos, "... (+%zu more)", len - shown);
    }
    ESP_LOGI(TAG, "%s %zu byte(s): %s", direction, len, line);
}

// ---------------------------------------------------------------------------
// Frame building / sending
// ---------------------------------------------------------------------------

void sendStatusFrame(uint8_t msg_type, uint8_t bitfield)
{
    // STX | msg_type | bitfield | crc8 | ETX
    uint8_t crc_input[2] = {msg_type, bitfield};
    uint8_t frame[5] = {STX, msg_type, bitfield, crc8(crc_input, 2), ETX};
    logHexBytes("TX", frame, sizeof(frame));
    ESP_LOGI(TAG, "TX status frame type=0x%02x bitfield=0x%02x crc=0x%02x (valid)",
             msg_type, bitfield, frame[3]);
    ports::send(frame, sizeof(frame));
}

// ---------------------------------------------------------------------------
// RX parser: byte-at-a-time state machine, handles both frame formats and
// arbitrary chunking by the transport.
// ---------------------------------------------------------------------------

enum class ParseState {
    WaitStx,
    ReadType,
    // request/response path
    ReadCmd, ReadLenHi, ReadLenLo, ReadPayload, ReadCrcHi, ReadCrcLo, ReadEtx,
    // compact status path (type had the top bit set)
    ReadBitfield, ReadStatusCrc, ReadStatusEtx,
};

ParseState s_state = ParseState::WaitStx;
uint8_t s_type = 0, s_cmd = 0, s_bitfield = 0, s_status_crc = 0;
uint16_t s_len = 0, s_crc = 0;
std::vector<uint8_t> s_payload;

void resetParser()
{
    s_state = ParseState::WaitStx;
    s_payload.clear();
}

void dispatchRequestFrame()
{
    uint16_t computed = crc16(&s_type, 1);
    computed = crc16(&s_cmd, 1, computed);
    uint8_t len_bytes[2] = {static_cast<uint8_t>(s_len >> 8), static_cast<uint8_t>(s_len & 0xFF)};
    computed = crc16(len_bytes, 2, computed);
    computed = crc16(s_payload.data(), s_payload.size(), computed);

    if (computed != s_crc) {
        ESP_LOGW(TAG, "RX frame type=0x%02x cmd=0x%02x CRC INVALID (got 0x%04x, expected 0x%04x) -- dropped",
                 s_type, s_cmd, s_crc, computed);
        return;
    }
    ESP_LOGI(TAG, "RX frame type=0x%02x cmd=0x%02x len=%u crc=0x%04x (valid)",
             s_type, s_cmd, s_len, s_crc);

    switch (s_type) {
    case TYPE_CONTROL:
        if (s_cmd == CMD_DIAL_SET && s_payload.size() == 1 && s_dial_cb) {
            s_dial_cb(s_payload[0]);
        }
        break;
    case TYPE_UPDATE:
        if (s_update_cb) {
            s_update_cb(s_cmd, s_payload.data(), s_payload.size());
        }
        break;
    default:
        ESP_LOGW(TAG, "RX unknown frame type 0x%02x, ignoring", s_type);
        break;
    }
}

void dispatchStatusFrame()
{
    uint8_t crc_input[2] = {s_type, s_bitfield};
    uint8_t computed = crc8(crc_input, 2);
    if (computed != s_status_crc) {
        ESP_LOGW(TAG, "RX status frame type=0x%02x CRC INVALID (got 0x%02x, expected 0x%02x) -- dropped",
                 s_type, s_status_crc, computed);
        return;
    }
    ESP_LOGI(TAG, "RX status frame type=0x%02x bitfield=0x%02x crc=0x%02x (valid)",
             s_type, s_bitfield, s_status_crc);

    if (s_type == STATUS_MSG_TOGGLES && s_toggles_cb) {
        s_toggles_cb(s_bitfield);
    }
}

void feedByte(uint8_t b)
{
    switch (s_state) {
    case ParseState::WaitStx:
        if (b == STX) {
            s_state = ParseState::ReadType;
        }
        break;

    case ParseState::ReadType:
        s_type = b;
        // Top bit set = compact status frame; clear = request/response.
        s_state = (b & 0x80) ? ParseState::ReadBitfield : ParseState::ReadCmd;
        break;

    // --- request/response path ---
    case ParseState::ReadCmd:
        s_cmd = b;
        s_state = ParseState::ReadLenHi;
        break;
    case ParseState::ReadLenHi:
        s_len = static_cast<uint16_t>(b) << 8;
        s_state = ParseState::ReadLenLo;
        break;
    case ParseState::ReadLenLo:
        s_len |= b;
        if (s_len > MAX_PAYLOAD) {
            ESP_LOGW(TAG, "RX frame len %u > max %zu -- resyncing", s_len, MAX_PAYLOAD);
            resetParser();
            break;
        }
        s_payload.clear();
        s_state = (s_len == 0) ? ParseState::ReadCrcHi : ParseState::ReadPayload;
        break;
    case ParseState::ReadPayload:
        s_payload.push_back(b);
        if (s_payload.size() >= s_len) {
            s_state = ParseState::ReadCrcHi;
        }
        break;
    case ParseState::ReadCrcHi:
        s_crc = static_cast<uint16_t>(b) << 8;
        s_state = ParseState::ReadCrcLo;
        break;
    case ParseState::ReadCrcLo:
        s_crc |= b;
        s_state = ParseState::ReadEtx;
        break;
    case ParseState::ReadEtx:
        if (b == ETX) {
            dispatchRequestFrame();
        } else {
            ESP_LOGW(TAG, "RX frame missing ETX -- dropped");
        }
        resetParser();
        break;

    // --- compact status path ---
    case ParseState::ReadBitfield:
        s_bitfield = b;
        s_state = ParseState::ReadStatusCrc;
        break;
    case ParseState::ReadStatusCrc:
        s_status_crc = b;
        s_state = ParseState::ReadStatusEtx;
        break;
    case ParseState::ReadStatusEtx:
        if (b == ETX) {
            dispatchStatusFrame();
        } else {
            ESP_LOGW(TAG, "RX status frame missing ETX -- dropped");
        }
        resetParser();
        break;
    }
}

void onPortData(const uint8_t *data, size_t len)
{
    logHexBytes("RX", data, len);
    for (size_t i = 0; i < len; i++) {
        feedByte(data[i]);
    }
}

} // namespace

void init()
{
    ports::onReceive(onPortData);
}

void sendFrame(uint8_t type, uint8_t cmd, const uint8_t *payload, size_t len)
{
    // STX | type | cmd | len_hi | len_lo | payload | crc_hi | crc_lo | ETX
    std::vector<uint8_t> frame;
    frame.reserve(9 + len);
    frame.push_back(STX);
    frame.push_back(type);
    frame.push_back(cmd);
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    for (size_t i = 0; i < len; i++) {
        frame.push_back(payload[i]);
    }
    // CRC over type..payload (i.e. everything between STX and the CRC).
    uint16_t crc = crc16(frame.data() + 1, frame.size() - 1);
    frame.push_back(static_cast<uint8_t>(crc >> 8));
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(ETX);

    logHexBytes("TX", frame.data(), frame.size());
    ESP_LOGI(TAG, "TX frame type=0x%02x cmd=0x%02x len=%zu crc=0x%04x (valid)",
             type, cmd, len, crc);
    ports::send(frame.data(), frame.size());
}

void sendDial(uint8_t value)
{
    sendFrame(TYPE_CONTROL, CMD_DIAL_SET, &value, 1);
}

void sendToggles(uint8_t bitfield)
{
    sendStatusFrame(STATUS_MSG_TOGGLES, bitfield);
}

void onDialReceived(DialCallback cb)
{
    s_dial_cb = std::move(cb);
}

void onTogglesReceived(TogglesCallback cb)
{
    s_toggles_cb = std::move(cb);
}

void onUpdateFrame(FrameCallback cb)
{
    s_update_cb = std::move(cb);
}

} // namespace comm_protocol
