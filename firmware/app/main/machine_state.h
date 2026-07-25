#pragma once

#include <cstdint>
#include <functional>

/**
 * Single source of truth for the "Machine" scene's controls: a dial, a
 * slider, a mode selector, a signed setpoint, a bank of on/off toggles and
 * a momentary pulse button. Three different things can change this state,
 * and whichever one does, the others are kept in sync automatically:
 *
 *   1. The local touchscreen (ui_machine.cpp calls the set*() functions
 *      with Source::LocalUi when the user drags the dial, flips a switch,
 *      picks a mode, ...). This sends an RS485 packet to the other board
 *      AND broadcasts the change over WebSocket to any connected browsers.
 *
 *   2. A frame arriving from another identical board over the active
 *      Ports transport (this module subscribes to comm_protocol's
 *      callbacks itself). This updates the
 *      local LVGL widgets AND broadcasts over WebSocket -- but does NOT
 *      send its own RS485 packet back out, which is what prevents an
 *      infinite echo loop between two connected boards.
 *
 *   3. A command from a WebSocket-connected browser (web_server.cpp calls
 *      the same set*() functions with Source::Web). This is treated exactly
 *      like a local touchscreen action: it updates the LVGL widgets AND
 *      sends an RS485 packet, because per the product requirement,
 *      "anything done on the web GUI behaves on the ESP32 as if it was done
 *      on the display itself". The only difference between the two local
 *      sources is whether the on-screen widget already shows the new value
 *      (it does when the user's own finger just moved it).
 *
 * This module has no LVGL or networking code of its own -- it just holds
 * the state and calls out to small callback hooks, which keeps it easy to
 * reason about and easy to test independently of the UI/network layers.
 */
namespace machine_state {

// --- Control ranges, shared by the touchscreen and the web UI, and used
//     to clamp anything arriving from the wire. -------------------------
constexpr uint8_t DIAL_MAX = 100;
constexpr uint16_t SPEED_MAX = 1000;
constexpr int16_t SETPOINT_MIN = -50;
constexpr int16_t SETPOINT_MAX = 150;
constexpr int16_t SETPOINT_STEP = 5;

/** On/off devices, one bit each in the compact status bitfield (see
 *  comm_protocol.h). Bits 4..7 are free for more switches. */
enum ToggleId : uint8_t {
    TOGGLE_SAMPLE = 0,
    TOGGLE_PUMP = 1,
    TOGGLE_VALVE = 2,
    TOGGLE_LIGHT = 3,
    TOGGLE_COUNT = 4,
};

/** Mode selector positions. */
enum ModeId : uint8_t {
    MODE_IDLE = 0,
    MODE_MANUAL = 1,
    MODE_AUTO = 2,
    MODE_SERVICE = 3,
    MODE_COUNT = 4,
};

/** Id of the momentary "pulse" button. The wire format carries a button id
 *  so more momentary buttons can be added without a new message type. */
constexpr uint8_t PULSE_BUTTON_MAIN = 0;

struct State {
    uint8_t dial_value = 0;
    uint16_t speed = 0;
    int16_t setpoint = 0;
    uint8_t mode = MODE_IDLE;
    bool toggles[TOGGLE_COUNT] = {};
    /** How many pulse presses have happened anywhere on the link. Both
     *  boards count the same presses (the presser counts its own, the peer
     *  counts the frame it receives), so the counters stay in step. */
    uint32_t pulse_count = 0;
};

/** Where a change came from. Both are "local actions" that transmit; they
 *  differ only in whether the touchscreen widget already shows the value. */
enum class Source {
    LocalUi,
    Web,
};

/** Display names, shared by the touchscreen UI and the logs. */
const char *toggleName(uint8_t id);
const char *modeName(uint8_t mode);

/** Called to move the on-screen widgets when state changes for a reason
 *  OTHER than the local UI itself (i.e. from RS485 or the web). Registered
 *  once from ui_machine.cpp when the scene is built, and cleared again when
 *  it is destroyed. Any individual callback may be left empty. */
struct UiCallbacks {
    std::function<void(uint8_t value)> apply_dial;
    std::function<void(uint16_t value)> apply_speed;
    std::function<void(int16_t value)> apply_setpoint;
    std::function<void(uint8_t mode)> apply_mode;
    std::function<void(uint8_t id, bool state)> apply_toggle;
    /** A pulse press happened (locally or on the peer): flash the on-screen
     *  indicator and show the new count. */
    std::function<void(uint32_t count)> apply_pulse;
};
void setUiCallbacks(const UiCallbacks &callbacks);
void clearUiCallbacks();

/** Called on every state change, for anything that wants to observe it --
 *  web_server.cpp uses this to broadcast updates to WebSocket clients. */
using StateChangeCallback = std::function<void(const State &state)>;
void onStateChange(StateChangeCallback cb);

/** Hooks into comm_protocol's receive callbacks. Call once at startup. */
void init();

// --- Local actions (touchscreen or web): update the state, move the
//     widgets when needed, transmit to the peer, notify observers. Values
//     are clamped to the ranges above. ---------------------------------
void setDial(uint8_t value, Source source);
void setSpeed(uint16_t value, Source source);
void setSetpoint(int16_t value, Source source);
void setMode(uint8_t mode, Source source);
void setToggle(uint8_t id, bool state, Source source);
void firePulse(Source source);

State current();

} // namespace machine_state
