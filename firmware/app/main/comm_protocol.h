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
 *    Used for everything with request/response semantics: link control
 *    (baud switching), version exchange, firmware update streaming.
 *
 * 2) Compact status frame -- for device state that changes often and needs
 *    no request/response semantics. msg_type has the top bit set (>= 0x80)
 *    to distinguish it from a request/response frame's `type` at parse
 *    time, and also selects the data width:
 *
 *      STX | msg_type | data... | crc | ETX
 *
 *    - BITFIELD types (1 data byte): banks of on/off devices, one bit each
 *      (0x81 TOGGLES: bit 0 = the sample toggle).
 *    - BYTEFIELD types (2 data bytes, big-endian): one multi-byte value per
 *      control (0x82 DIAL: 0-100 today, room to 65535).
 *    - crc: CRC8 (poly 0x07, init 0x00) over msg_type + data bytes.
 *
 * Everything else -- packet messages, baud-rate changes, app version
 * exchange, streaming firmware updates -- uses the request/response frame.
 *
 * Every byte sent and received is logged in hex (0xNN ...), and every
 * frame's CRC verdict (TX and RX) is logged too, so a bus trace is always
 * one `./build.sh --run` away.
 */
namespace comm_protocol {

// --- Request/response frame constants (also used by fw_update.cpp) -------
constexpr uint8_t STX = 0x02;
constexpr uint8_t ETX = 0x03;

constexpr uint8_t TYPE_CONTROL = 0x01;  // link control (baud switching, ...)
constexpr uint8_t TYPE_UPDATE  = 0x02;  // firmware update transfer (fw_update.cpp)

constexpr uint8_t CMD_BAUD_SET = 0x02;  // payload: 4 bytes BE, new baud rate

// --- Compact status frame constants ---------------------------------------
// msg_type picks the data width (see statusDataLen() in comm_protocol.cpp):
//   0x81 TOGGLES: 1-byte BITFIELD -- banks of on/off devices, one bit each.
//                 Use this for the sample toggle and every future on/off
//                 device (bits 1..7 are free).
//   0x82 DIAL:    2-byte BYTEFIELD (big-endian) -- one multi-byte value per
//                 control. Use this pattern for the dial (0-100 today, up
//                 to 65535) and future value-carrying controls; give each
//                 its own msg_type.
constexpr uint8_t STATUS_MSG_TOGGLES = 0x81;
constexpr uint8_t STATUS_MSG_DIAL    = 0x82;

using DialCallback = std::function<void(uint16_t value)>;
using TogglesCallback = std::function<void(uint8_t bitfield)>;
using BaudCallback = std::function<void(uint32_t baud)>;
/** Raw hook for other modules (fw_update) to receive whole verified frames
 *  of their message type. */
using FrameCallback = std::function<void(uint8_t cmd, const uint8_t *payload, size_t len)>;

/** Hooks the parser into ports::onReceive(). Call once after ports::init(). */
void init();

// --- Sending ---------------------------------------------------------------
/** Dial value as a compact 2-byte bytefield status frame (0x82). */
void sendDial(uint16_t value);
/** On/off bank as a compact 1-byte bitfield status frame (0x81). */
void sendToggles(uint8_t bitfield);
/** Broadcasts "switch to this baud NOW" (sent at the CURRENT baud so the
 *  peer hears it, then both sides switch -- see ports::setBaud). */
void sendBaudChange(uint32_t baud);
/** Generic request/response frame send (used by fw_update.cpp). */
void sendFrame(uint8_t type, uint8_t cmd, const uint8_t *payload, size_t len);

// --- Receiving -------------------------------------------------------------
void onDialReceived(DialCallback cb);
void onTogglesReceived(TogglesCallback cb);
void onBaudChangeReceived(BaudCallback cb);
/** Receive every verified TYPE_UPDATE frame. */
void onUpdateFrame(FrameCallback cb);

/** CRC16-CCITT helper, exposed for fw_update's whole-image check. */
uint16_t crc16(const uint8_t *data, size_t len, uint16_t seed = 0xFFFF);

} // namespace comm_protocol
