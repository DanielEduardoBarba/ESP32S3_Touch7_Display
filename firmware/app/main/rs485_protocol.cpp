#include "rs485_protocol.h"

#include <cstdint>

#include "esp_log.h"
#include "rs485.h"

namespace rs485_protocol {
namespace {

const char *TAG = "rs485_protocol";

// --- Wire format constants (see header for the full frame layout) ---
constexpr uint8_t SOF = 0xAA;
constexpr uint8_t EOF_BYTE = 0x55;
constexpr uint8_t MSG_TYPE_DIAL_SET = 0x01;
constexpr uint8_t MSG_TYPE_TOGGLE_SET = 0x02;
constexpr uint8_t MAX_PAYLOAD_LEN = 8; // generous headroom; real payloads are 1-2 bytes

DialCallback s_dial_cb;
ToggleCallback s_toggle_cb;

/**
 * Computes CRC-8 (polynomial 0x07, initial value 0x00) one byte at a time.
 * This is the same simple CRC-8 used by many embedded serial protocols --
 * cheap to compute, and enough to catch single-bit line noise on a short,
 * point-to-point RS485 run.
 */
uint8_t crc8Update(uint8_t crc, uint8_t data)
{
    crc ^= data;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

uint8_t computeCrc(uint8_t msg_type, uint8_t len, const uint8_t *payload)
{
    uint8_t crc = 0;
    crc = crc8Update(crc, msg_type);
    crc = crc8Update(crc, len);
    for (uint8_t i = 0; i < len; i++) {
        crc = crc8Update(crc, payload[i]);
    }
    return crc;
}

/** Dispatches a fully-received, CRC-verified frame to the matching callback. */
void dispatchFrame(uint8_t msg_type, const uint8_t *payload, uint8_t len)
{
    switch (msg_type) {
    case MSG_TYPE_DIAL_SET:
        if (len == 1 && s_dial_cb) {
            s_dial_cb(payload[0]);
        }
        break;
    case MSG_TYPE_TOGGLE_SET:
        if (len == 2 && s_toggle_cb) {
            s_toggle_cb(payload[0], payload[1] != 0);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unknown message type 0x%02x, ignoring", msg_type);
        break;
    }
}

/**
 * Simple byte-at-a-time frame parser. RS485 delivers bytes to us in
 * arbitrary-sized chunks (see the RxCallback below), so we can't assume a
 * whole frame arrives in one callback -- this small state machine lets us
 * pick up a frame no matter how it gets split across reads.
 */
enum class ParseState {
    WaitSof,
    ReadType,
    ReadLen,
    ReadPayload,
    ReadCrc,
    WaitEof,
};

ParseState s_state = ParseState::WaitSof;
uint8_t s_msg_type = 0;
uint8_t s_payload_len = 0;
uint8_t s_payload[MAX_PAYLOAD_LEN];
uint8_t s_payload_index = 0;
uint8_t s_received_crc = 0;

void resetParser()
{
    s_state = ParseState::WaitSof;
    s_payload_index = 0;
}

void feedByte(uint8_t b)
{
    switch (s_state) {
    case ParseState::WaitSof:
        if (b == SOF) {
            s_state = ParseState::ReadType;
        }
        break;

    case ParseState::ReadType:
        s_msg_type = b;
        s_state = ParseState::ReadLen;
        break;

    case ParseState::ReadLen:
        s_payload_len = b;
        if (s_payload_len > MAX_PAYLOAD_LEN) {
            // Corrupt/unexpected frame -- bail out and wait for the next SOF.
            resetParser();
            break;
        }
        s_payload_index = 0;
        s_state = (s_payload_len == 0) ? ParseState::ReadCrc : ParseState::ReadPayload;
        break;

    case ParseState::ReadPayload:
        s_payload[s_payload_index++] = b;
        if (s_payload_index >= s_payload_len) {
            s_state = ParseState::ReadCrc;
        }
        break;

    case ParseState::ReadCrc:
        s_received_crc = b;
        s_state = ParseState::WaitEof;
        break;

    case ParseState::WaitEof:
        if (b == EOF_BYTE) {
            uint8_t expected_crc = computeCrc(s_msg_type, s_payload_len, s_payload);
            if (expected_crc == s_received_crc) {
                dispatchFrame(s_msg_type, s_payload, s_payload_len);
            } else {
                ESP_LOGW(TAG, "CRC mismatch (got 0x%02x, expected 0x%02x), dropping frame",
                         s_received_crc, expected_crc);
            }
        } else {
            ESP_LOGW(TAG, "Missing EOF marker, dropping frame");
        }
        resetParser();
        break;
    }
}

void onRs485Data(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        feedByte(data[i]);
    }
}

/** Builds and sends a complete frame for the given type/payload. */
void sendFrame(uint8_t msg_type, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[3 + MAX_PAYLOAD_LEN + 2];
    size_t i = 0;
    frame[i++] = SOF;
    frame[i++] = msg_type;
    frame[i++] = len;
    for (uint8_t j = 0; j < len; j++) {
        frame[i++] = payload[j];
    }
    frame[i++] = computeCrc(msg_type, len, payload);
    frame[i++] = EOF_BYTE;
    rs485::send(frame, i);
}

} // namespace

void init()
{
    rs485::onReceive(onRs485Data);
}

void sendDial(uint8_t value)
{
    uint8_t payload[1] = {value};
    sendFrame(MSG_TYPE_DIAL_SET, payload, 1);
}

void sendToggle(uint8_t toggle_id, bool state)
{
    uint8_t payload[2] = {toggle_id, static_cast<uint8_t>(state ? 1 : 0)};
    sendFrame(MSG_TYPE_TOGGLE_SET, payload, 2);
}

void onDialReceived(DialCallback cb)
{
    s_dial_cb = std::move(cb);
}

void onToggleReceived(ToggleCallback cb)
{
    s_toggle_cb = std::move(cb);
}

} // namespace rs485_protocol
