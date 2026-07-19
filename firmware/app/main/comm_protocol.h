#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

/**
 * Standard framing for ALL peer-to-peer communication (over whichever
 * transport ports.cpp has active -- RS485 by default).
 *
 * 1) Request/response frame -- used for everything with a payload:
 *
 *      STX | type | cmd | len_hi | len_lo | payload... | crc_hi | crc_lo | ETX
 *
 *    - STX = 0x02, ETX = 0x03
 *    - type: message family (TYPE_*)
 *    - cmd:  command within the family (CMD_*)
 *    - len:  16-bit big-endian payload length
 *    - crc:  CRC16-CCITT (poly 0x1021, init 0xFFFF) over type..payload,
 *            big-endian
 *
 *    Multi-byte values (like the dial) ride in the payload as plain bytes.
 *
 * 2) Compact status frame -- for banks of on/off devices, one bit each:
 *
 *      STX | msg_type | bitfield | crc | ETX
 *
 *    - msg_type has the top bit set (>= 0x80) to distinguish it from a
 *      request/response frame's `type` at parse time
 *    - bitfield: 8 on/off states (bit 0 = toggle 0, ...)
 *    - crc: CRC8 (poly 0x07, init 0x00) over msg_type + bitfield
 *
 * Every byte sent and received is logged in hex (0xNN ...), and every
 * frame's CRC verdict (TX and RX) is logged too, so a bus trace is always
 * one `./build.sh --run` away.
 */
namespace comm_protocol {

// --- Request/response frame constants (also used by fw_update.cpp) -------
constexpr uint8_t STX = 0x02;
constexpr uint8_t ETX = 0x03;

constexpr uint8_t TYPE_CONTROL = 0x01;  // machine controls (dial, ...)
constexpr uint8_t TYPE_UPDATE  = 0x02;  // firmware update transfer (fw_update.cpp)

constexpr uint8_t CMD_DIAL_SET = 0x01;  // payload: 1 byte, 0-100

// --- Compact status frame constants ---------------------------------------
constexpr uint8_t STATUS_MSG_TOGGLES = 0x81; // bit 0 = the sample toggle

using DialCallback = std::function<void(uint8_t value)>;
using TogglesCallback = std::function<void(uint8_t bitfield)>;
/** Raw hook for other modules (fw_update) to receive whole verified frames
 *  of their message type. */
using FrameCallback = std::function<void(uint8_t cmd, const uint8_t *payload, size_t len)>;

/** Hooks the parser into ports::onReceive(). Call once after ports::init(). */
void init();

// --- Sending ---------------------------------------------------------------
void sendDial(uint8_t value);
void sendToggles(uint8_t bitfield);
/** Generic request/response frame send (used by fw_update.cpp). */
void sendFrame(uint8_t type, uint8_t cmd, const uint8_t *payload, size_t len);

// --- Receiving -------------------------------------------------------------
void onDialReceived(DialCallback cb);
void onTogglesReceived(TogglesCallback cb);
/** Receive every verified TYPE_UPDATE frame. */
void onUpdateFrame(FrameCallback cb);

/** CRC16-CCITT helper, exposed for fw_update's whole-image check. */
uint16_t crc16(const uint8_t *data, size_t len, uint16_t seed = 0xFFFF);

} // namespace comm_protocol
