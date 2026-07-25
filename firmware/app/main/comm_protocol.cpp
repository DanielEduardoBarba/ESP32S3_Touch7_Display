#include "comm_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "app_config.h"
#include "ports.h"

namespace comm_protocol {
namespace {

const char *TAG = "comm";

// Payload ceiling: fits the largest firmware-update chunk plus headroom,
// while bounding the parser's buffer. (len is 16-bit on the wire.)
constexpr size_t MAX_PAYLOAD = APP_COMM_MAX_PAYLOAD;

DialCallback s_dial_cb;
TogglesCallback s_toggles_cb;
SpeedCallback s_speed_cb;
ModeCallback s_mode_cb;
SetpointCallback s_setpoint_cb;
PulseCallback s_pulse_cb;
BaudCallback s_baud_cb;
WifiCredsCallback s_wifi_creds_cb;
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
    // CRC16-CCITT (poly 0x1021), table-driven: ~8x faster than the bitwise
    // loop, which matters when fw_update runs it over multi-MB images.
    // Also usable incrementally by passing the previous result as `seed`.
    static uint16_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint16_t i = 0; i < 256; i++) {
            uint16_t c = static_cast<uint16_t>(i << 8);
            for (int b = 0; b < 8; b++) {
                c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021)
                                  : static_cast<uint16_t>(c << 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }
    uint16_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc = static_cast<uint16_t>((crc << 8) ^ table[(crc >> 8) ^ data[i]]);
    }
    return crc;
}

namespace {

// ---------------------------------------------------------------------------
// Hex logging
// ---------------------------------------------------------------------------

/** Logs a byte run as "0x02 0x01 ..." lines at DEBUG level. During a
 *  firmware transfer a 1-2KB chunk arrives as ~10 UART deliveries -- at
 *  INFO these hex dumps alone throttled transfers to a fraction of the
 *  line rate. The frame-summary lines (type/cmd/len/CRC verdict) stay at
 *  INFO, so the bus remains fully auditable; raise `comm` to DEBUG
 *  (esp_log_level_set) to see raw bytes again. */
void logHexBytes(const char *direction, const uint8_t *data, size_t len)
{
    if (esp_log_level_get(TAG) < ESP_LOG_DEBUG) {
        return; // skip the formatting cost entirely when not shown
    }
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

void sendStatusFrame(uint8_t msg_type, const uint8_t *data, size_t data_len)
{
    // STX | msg_type | data... | crc8 | ETX
    uint8_t frame[3 + 4]; // enough for the largest status data width (2)
    size_t pos = 0;
    frame[pos++] = STX;
    frame[pos++] = msg_type;
    for (size_t i = 0; i < data_len; i++) {
        frame[pos++] = data[i];
    }
    // CRC over msg_type + data.
    uint8_t crc_input[1 + 4];
    crc_input[0] = msg_type;
    std::memcpy(crc_input + 1, data, data_len);
    frame[pos++] = crc8(crc_input, 1 + data_len);
    frame[pos++] = ETX;

    logHexBytes("TX", frame, pos);
    ESP_LOGI(TAG, "TX status frame type=0x%02x data_len=%zu crc=0x%02x (valid)",
             msg_type, data_len, frame[pos - 2]);
    ports::send(frame, pos);
}

/** Data width of a compact status frame, selected by its msg_type:
 *  bitfield types carry 1 byte, bytefield types carry 2. Returns 0 for
 *  unknown types (parser resyncs). */
size_t statusDataLen(uint8_t msg_type)
{
    switch (msg_type) {
    case STATUS_MSG_TOGGLES:  return 1; // bitfield: 8 on/off devices
    case STATUS_MSG_DIAL:     return 2; // bytefield: one 16-bit value
    case STATUS_MSG_SPEED:    return 2;
    case STATUS_MSG_MODE:     return 1;
    case STATUS_MSG_SETPOINT: return 2; // signed int16
    case STATUS_MSG_PULSE:    return 1; // button id
    default:                  return 0;
    }
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
    ReadStatusData, ReadStatusCrc, ReadStatusEtx,
};

ParseState s_state = ParseState::WaitStx;
uint8_t s_type = 0, s_cmd = 0, s_status_crc = 0;
uint8_t s_status_data[4];
size_t s_status_len = 0, s_status_got = 0;
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
        if (s_cmd == CMD_BAUD_SET && s_payload.size() == 4 && s_baud_cb) {
            uint32_t baud = (uint32_t)s_payload[0] << 24 | (uint32_t)s_payload[1] << 16 |
                            (uint32_t)s_payload[2] << 8 | s_payload[3];
            s_baud_cb(baud);
        } else if (s_cmd == CMD_WIFI_CREDS && s_wifi_creds_cb) {
            // ssid_len | ssid | pass_len | pass, all bounds-checked against
            // the payload actually received (it came off a wire).
            const uint8_t *p = s_payload.data();
            size_t n = s_payload.size();
            if (n < 1) {
                break;
            }
            size_t ssid_len = p[0];
            if (n < 1 + ssid_len + 1) {
                ESP_LOGW(TAG, "RX wifi creds frame truncated -- dropped");
                break;
            }
            size_t pass_len = p[1 + ssid_len];
            if (n < 1 + ssid_len + 1 + pass_len || ssid_len > 32 || pass_len > 64) {
                ESP_LOGW(TAG, "RX wifi creds frame malformed -- dropped");
                break;
            }
            char ssid[33] = {};
            char pass[65] = {};
            std::memcpy(ssid, p + 1, ssid_len);
            std::memcpy(pass, p + 1 + ssid_len + 1, pass_len);
            s_wifi_creds_cb(ssid, pass);
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
    uint8_t crc_input[1 + 4];
    crc_input[0] = s_type;
    std::memcpy(crc_input + 1, s_status_data, s_status_len);
    uint8_t computed = crc8(crc_input, 1 + s_status_len);
    if (computed != s_status_crc) {
        ESP_LOGW(TAG, "RX status frame type=0x%02x CRC INVALID (got 0x%02x, expected 0x%02x) -- dropped",
                 s_type, s_status_crc, computed);
        return;
    }
    ESP_LOGI(TAG, "RX status frame type=0x%02x data_len=%zu crc=0x%02x (valid)",
             s_type, s_status_len, s_status_crc);

    if (s_type == STATUS_MSG_TOGGLES && s_toggles_cb) {
        s_toggles_cb(s_status_data[0]);
    } else if (s_type == STATUS_MSG_DIAL && s_dial_cb) {
        s_dial_cb((uint16_t)s_status_data[0] << 8 | s_status_data[1]);
    } else if (s_type == STATUS_MSG_SPEED && s_speed_cb) {
        s_speed_cb((uint16_t)s_status_data[0] << 8 | s_status_data[1]);
    } else if (s_type == STATUS_MSG_MODE && s_mode_cb) {
        s_mode_cb(s_status_data[0]);
    } else if (s_type == STATUS_MSG_SETPOINT && s_setpoint_cb) {
        // Two's complement on the wire: rebuild as unsigned, then cast.
        uint16_t raw = (uint16_t)s_status_data[0] << 8 | s_status_data[1];
        s_setpoint_cb(static_cast<int16_t>(raw));
    } else if (s_type == STATUS_MSG_PULSE && s_pulse_cb) {
        s_pulse_cb(s_status_data[0]);
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
        if (b & 0x80) {
            // Compact status frame; data width is a function of msg_type.
            s_status_len = statusDataLen(b);
            s_status_got = 0;
            if (s_status_len == 0) {
                ESP_LOGW(TAG, "RX unknown status msg_type 0x%02x -- resyncing", b);
                resetParser();
                break;
            }
            s_state = ParseState::ReadStatusData;
        } else {
            s_state = ParseState::ReadCmd;
        }
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
    case ParseState::ReadStatusData:
        s_status_data[s_status_got++] = b;
        if (s_status_got >= s_status_len) {
            s_state = ParseState::ReadStatusCrc;
        }
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
    if (len == 0) {
        // Stream-break marker from the transport (RX overflow flush, or an
        // idle gap on the wire): any in-flight frame is dead. Only worth
        // acting on when the parser is actually mid-frame.
        if (s_state != ParseState::WaitStx) {
            ESP_LOGW(TAG, "RX stream break -- parser reset, waiting for next frame");
            resetParser();
        }
        return;
    }
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

void sendDial(uint16_t value)
{
    // 2-byte BYTEFIELD status frame: dial is a value-carrying control, so
    // it gets two bytes (room to 65535) instead of a bit.
    uint8_t data[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
    sendStatusFrame(STATUS_MSG_DIAL, data, sizeof(data));
}

void sendBaudChange(uint32_t baud)
{
    uint8_t payload[4] = {
        static_cast<uint8_t>(baud >> 24), static_cast<uint8_t>(baud >> 16),
        static_cast<uint8_t>(baud >> 8), static_cast<uint8_t>(baud),
    };
    sendFrame(TYPE_CONTROL, CMD_BAUD_SET, payload, sizeof(payload));
}

void sendToggles(uint8_t bitfield)
{
    // 1-byte BITFIELD status frame: on/off devices, one bit each.
    sendStatusFrame(STATUS_MSG_TOGGLES, &bitfield, 1);
}

void sendSpeed(uint16_t value)
{
    uint8_t data[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
    sendStatusFrame(STATUS_MSG_SPEED, data, sizeof(data));
}

void sendMode(uint8_t mode)
{
    sendStatusFrame(STATUS_MSG_MODE, &mode, 1);
}

void sendSetpoint(int16_t value)
{
    // Sent as two's complement big-endian so negative setpoints survive
    // the trip (the receiver casts the raw 16 bits back to int16_t).
    uint16_t raw = static_cast<uint16_t>(value);
    uint8_t data[2] = {static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw & 0xFF)};
    sendStatusFrame(STATUS_MSG_SETPOINT, data, sizeof(data));
}

void sendPulse(uint8_t button_id)
{
    sendStatusFrame(STATUS_MSG_PULSE, &button_id, 1);
}

void sendWifiCredentials(const char *ssid, const char *password)
{
    size_t ssid_len = std::strlen(ssid);
    size_t pass_len = std::strlen(password);
    if (ssid_len > 32 || pass_len > 64) {
        ESP_LOGW(TAG, "Refusing to send oversized wifi credentials (ssid %zu, pass %zu)",
                 ssid_len, pass_len);
        return;
    }
    std::vector<uint8_t> payload;
    payload.reserve(2 + ssid_len + pass_len);
    payload.push_back(static_cast<uint8_t>(ssid_len));
    payload.insert(payload.end(), ssid, ssid + ssid_len);
    payload.push_back(static_cast<uint8_t>(pass_len));
    payload.insert(payload.end(), password, password + pass_len);
    // The password is never logged, here or anywhere else on this path.
    ESP_LOGI(TAG, "TX wifi credentials for SSID '%s' to peer", ssid);
    sendFrame(TYPE_CONTROL, CMD_WIFI_CREDS, payload.data(), payload.size());
}

void onDialReceived(DialCallback cb)
{
    s_dial_cb = std::move(cb);
}

void onTogglesReceived(TogglesCallback cb)
{
    s_toggles_cb = std::move(cb);
}

void onSpeedReceived(SpeedCallback cb)
{
    s_speed_cb = std::move(cb);
}

void onModeReceived(ModeCallback cb)
{
    s_mode_cb = std::move(cb);
}

void onSetpointReceived(SetpointCallback cb)
{
    s_setpoint_cb = std::move(cb);
}

void onPulseReceived(PulseCallback cb)
{
    s_pulse_cb = std::move(cb);
}

void onBaudChangeReceived(BaudCallback cb)
{
    s_baud_cb = std::move(cb);
}

void onWifiCredentialsReceived(WifiCredsCallback cb)
{
    s_wifi_creds_cb = std::move(cb);
}

void onUpdateFrame(FrameCallback cb)
{
    s_update_cb = std::move(cb);
}

} // namespace comm_protocol
