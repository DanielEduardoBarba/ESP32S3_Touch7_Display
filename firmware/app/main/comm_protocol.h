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
 *      (0x81 TOGGLES: bits 0..3 = the four sample switches).
 *    - BYTEFIELD types (1 or 2 data bytes, big-endian): one value per
 *      control (0x82 DIAL, 0x83 SPEED, 0x85 SETPOINT are 16-bit; 0x84 MODE
 *      and 0x86 PULSE are 8-bit).
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
// payload: ssid_len(1) | ssid | pass_len(1) | pass -- see sendWifiCredentials()
constexpr uint8_t CMD_WIFI_CREDS = 0x03;

// --- Compact status frame constants ---------------------------------------
// msg_type picks the data width (see statusDataLen() in comm_protocol.cpp):
//   0x81 TOGGLES:  1-byte BITFIELD -- a bank of on/off devices, one bit each
//                  (bits 0..3 used today, bits 4..7 free for more switches).
//   0x82 DIAL:     2-byte BYTEFIELD (big-endian), 0-100 today.
//   0x83 SPEED:    2-byte BYTEFIELD, slider value.
//   0x84 MODE:     1-byte BYTEFIELD, enum selection.
//   0x85 SETPOINT: 2-byte BYTEFIELD carrying a SIGNED int16 (two's
//                  complement, big-endian) -- can go negative.
//   0x86 PULSE:    1-byte BYTEFIELD, momentary "button pressed" event
//                  (the byte is the button id) rather than a held state.
// Give every future value-carrying control its own msg_type in this range.
constexpr uint8_t STATUS_MSG_TOGGLES  = 0x81;
constexpr uint8_t STATUS_MSG_DIAL     = 0x82;
constexpr uint8_t STATUS_MSG_SPEED    = 0x83;
constexpr uint8_t STATUS_MSG_MODE     = 0x84;
constexpr uint8_t STATUS_MSG_SETPOINT = 0x85;
constexpr uint8_t STATUS_MSG_PULSE    = 0x86;

using DialCallback = std::function<void(uint16_t value)>;
using TogglesCallback = std::function<void(uint8_t bitfield)>;
using SpeedCallback = std::function<void(uint16_t value)>;
using ModeCallback = std::function<void(uint8_t mode)>;
using SetpointCallback = std::function<void(int16_t value)>;
using PulseCallback = std::function<void(uint8_t button_id)>;
using BaudCallback = std::function<void(uint32_t baud)>;
using WifiCredsCallback = std::function<void(const char *ssid, const char *password)>;
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
/** Slider value as a compact 2-byte bytefield status frame (0x83). */
void sendSpeed(uint16_t value);
/** Mode selection as a compact 1-byte bytefield status frame (0x84). */
void sendMode(uint8_t mode);
/** Signed setpoint as a compact 2-byte bytefield status frame (0x85). */
void sendSetpoint(int16_t value);
/** Momentary button press as a compact 1-byte status frame (0x86). */
void sendPulse(uint8_t button_id);
/** Broadcasts "switch to this baud NOW" (sent at the CURRENT baud so the
 *  peer hears it, then both sides switch -- see ports::setBaud). */
void sendBaudChange(uint32_t baud);
/** Hands this board's Wi-Fi credentials to the peer so it can join the same
 *  network (see wifi_sync.h). Length-prefixed rather than NUL-terminated so
 *  the frame stays self-describing:
 *      ssid_len(1) | ssid bytes | pass_len(1) | pass bytes
 *  NOTE: the payload is NOT encrypted -- the peer link is a short private
 *  wire between two boards of the same machine, but treat it as you would
 *  any other cable carrying secrets. */
void sendWifiCredentials(const char *ssid, const char *password);
/** Generic request/response frame send (used by fw_update.cpp). */
void sendFrame(uint8_t type, uint8_t cmd, const uint8_t *payload, size_t len);

// --- Receiving -------------------------------------------------------------
void onDialReceived(DialCallback cb);
void onTogglesReceived(TogglesCallback cb);
void onSpeedReceived(SpeedCallback cb);
void onModeReceived(ModeCallback cb);
void onSetpointReceived(SetpointCallback cb);
void onPulseReceived(PulseCallback cb);
void onBaudChangeReceived(BaudCallback cb);
void onWifiCredentialsReceived(WifiCredsCallback cb);
/** Receive every verified TYPE_UPDATE frame. */
void onUpdateFrame(FrameCallback cb);

/** CRC16-CCITT helper, exposed for fw_update's whole-image check. */
uint16_t crc16(const uint8_t *data, size_t len, uint16_t seed = 0xFFFF);

} // namespace comm_protocol
