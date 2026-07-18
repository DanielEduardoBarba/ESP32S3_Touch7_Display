#pragma once

#include <cstdint>
#include <functional>

/**
 * "Machine sync" packet protocol -- a tiny, fast binary protocol layered on
 * top of rs485.h, used to keep two identical boards' "Machine" scene (dial +
 * toggle) in sync over the RS485 bus.
 *
 * ---------------------------------------------------------------------------
 * Wire format (one frame per user action -- a dial release or a toggle flip):
 * ---------------------------------------------------------------------------
 *
 *   byte 0    : SOF          0xAA  (start-of-frame marker)
 *   byte 1    : MSG_TYPE     0x01 = DIAL_SET, 0x02 = TOGGLE_SET
 *   byte 2    : PAYLOAD_LEN  number of payload bytes that follow
 *   byte 3..N : PAYLOAD
 *                 DIAL_SET   : [ value ]             1 byte,  0-100
 *                 TOGGLE_SET : [ toggle_id, state ]  2 bytes, state is 0/1
 *   byte N+1  : CRC8         over MSG_TYPE + LEN + PAYLOAD (poly 0x07, init 0)
 *   byte N+2  : EOF          0x55  (end-of-frame marker; lets the receiver
 *                                   resync if a byte gets dropped/corrupted)
 *
 * Frame sizes: DIAL_SET = 6 bytes, TOGGLE_SET = 7 bytes.
 *
 * RS485 runs at 921600 baud (see rs485.cpp) -- the fastest rate this board's
 * UART/transceiver combo has been confirmed reliable at (it's the same rate
 * used for flashing over the USB-UART bridge). At that speed a whole frame
 * takes well under 100 microseconds on the wire, so it's effectively
 * instantaneous compared to how fast a human can drag a dial or flip a
 * switch. If you see garbled frames on your specific cable run/transceiver,
 * lower RS485_BAUD_RATE in rs485.cpp -- both ends must use the same rate.
 *
 * Only one frame is ever "in flight" at a time (sent on release/change, not
 * continuously), so there's no need for bus arbitration/collision handling
 * for this simple point-to-point use case.
 */
namespace rs485_protocol {

/** Fired when a DIAL_SET frame is received from the bus. Value is 0-100. */
using DialCallback = std::function<void(uint8_t value)>;

/** Fired when a TOGGLE_SET frame is received from the bus. */
using ToggleCallback = std::function<void(uint8_t toggle_id, bool state)>;

/** Hooks into rs485::onReceive() to parse incoming bytes into frames. Call
 *  once, after rs485::init(). */
void init();

/** Sends a DIAL_SET frame with the given value (0-100). */
void sendDial(uint8_t value);

/** Sends a TOGGLE_SET frame for the given toggle id and new state. */
void sendToggle(uint8_t toggle_id, bool state);

void onDialReceived(DialCallback cb);
void onToggleReceived(ToggleCallback cb);

} // namespace rs485_protocol
