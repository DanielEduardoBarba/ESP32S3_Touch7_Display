#pragma once

#include <cstdint>
#include <functional>

/**
 * Single source of truth for the "Machine" scene's state (the dial value
 * and one sample toggle). Three different things can change this state,
 * and whichever one does, the others are kept in sync automatically:
 *
 *   1. The local touchscreen (ui_machine.cpp calls setDialFromLocalUi() /
 *      setToggleFromLocalUi() when the user drags the dial or flips the
 *      switch). This sends an RS485 packet to the other board AND
 *      broadcasts the change over WebSocket to any connected browsers.
 *
 *   2. A frame arriving from another identical board over the active
 *      Ports transport (this module subscribes to comm_protocol's
 *      callbacks itself). This updates the
 *      local LVGL widgets AND broadcasts over WebSocket -- but does NOT
 *      send its own RS485 packet back out, which is what prevents an
 *      infinite echo loop between two connected boards.
 *
 *   3. A command from a WebSocket-connected browser (web_server.cpp calls
 *      setDialFromWeb() / setToggleFromWeb() when a message arrives). This
 *      is treated exactly like a local touchscreen action: it updates the
 *      LVGL widgets AND sends an RS485 packet, because per the product
 *      requirement, "anything done on the web GUI behaves on the ESP32 as
 *      if it was done on the display itself".
 *
 * This module has no LVGL or networking code of its own -- it just holds
 * the state and calls out to small callback hooks, which keeps it easy to
 * reason about and easy to test independently of the UI/network layers.
 */
namespace machine_state {

struct State {
    uint8_t dial_value = 0;
    bool toggle_state = false;
};

/** Bit position of the one sample toggle in the compact status bitfield
 *  (see comm_protocol.h). Future toggles get bits 1..7. */
constexpr uint8_t TOGGLE_BIT_SAMPLE = 0;

/** Called to move the on-screen widgets when state changes for a reason
 *  OTHER than the local UI itself (i.e. from RS485 or the web). Register
 *  these once from ui_machine.cpp. */
using ApplyDialCallback = std::function<void(uint8_t value)>;
using ApplyToggleCallback = std::function<void(bool state)>;
void setUiCallbacks(ApplyDialCallback apply_dial, ApplyToggleCallback apply_toggle);

/** Called on every state change, for anything that wants to observe it --
 *  web_server.cpp uses this to broadcast updates to WebSocket clients. */
using StateChangeCallback = std::function<void(const State &state)>;
void onStateChange(StateChangeCallback cb);

/** Hooks into comm_protocol's receive callbacks. Call once at startup. */
void init();

/** The user dragged the dial / flipped the switch on THIS board's screen. */
void setDialFromLocalUi(uint8_t value);
void setToggleFromLocalUi(bool state);

/** A command arrived from a WebSocket-connected browser -- treated the same
 *  as a local touchscreen action (updates the screen + sends RS485). */
void setDialFromWeb(uint8_t value);
void setToggleFromWeb(bool state);

State current();

} // namespace machine_state
