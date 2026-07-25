#include "machine_state.h"

#include <algorithm>
#include <vector>

#include "esp_log.h"
#include "comm_protocol.h"

namespace machine_state {
namespace {

const char *TAG = "machine_state";

State s_state;

UiCallbacks s_ui;
std::vector<StateChangeCallback> s_state_change_cbs;

/** Packs all on/off devices into the compact status bitfield (one bit per
 *  toggle; bits 4..7 are free for future switches). */
uint8_t toggleBitfield()
{
    uint8_t bits = 0;
    for (uint8_t i = 0; i < TOGGLE_COUNT; i++) {
        if (s_state.toggles[i]) {
            bits |= static_cast<uint8_t>(1u << i);
        }
    }
    return bits;
}

void notifyStateChange()
{
    for (const auto &cb : s_state_change_cbs) {
        cb(s_state);
    }
}

// --- Frames from the peer board --------------------------------------------
// Move the local widgets to match, but do NOT transmit here -- doing so
// would immediately bounce the value back to the sender, forever. Only
// *local* actions (touchscreen or web) transmit.

void handleRemoteDial(uint16_t value)
{
    // Wire format is a 2-byte bytefield (room to 65535); this dial only
    // uses 0-100 of it.
    uint8_t clamped = value > DIAL_MAX ? DIAL_MAX : static_cast<uint8_t>(value);
    ESP_LOGI(TAG, "peer: dial set to %u", (unsigned)clamped);
    s_state.dial_value = clamped;
    if (s_ui.apply_dial) {
        s_ui.apply_dial(clamped);
    }
    notifyStateChange();
}

void handleRemoteSpeed(uint16_t value)
{
    uint16_t clamped = value > SPEED_MAX ? SPEED_MAX : value;
    ESP_LOGI(TAG, "peer: speed set to %u", (unsigned)clamped);
    s_state.speed = clamped;
    if (s_ui.apply_speed) {
        s_ui.apply_speed(clamped);
    }
    notifyStateChange();
}

void handleRemoteSetpoint(int16_t value)
{
    int16_t clamped = std::max(SETPOINT_MIN, std::min(SETPOINT_MAX, value));
    ESP_LOGI(TAG, "peer: setpoint set to %d", (int)clamped);
    s_state.setpoint = clamped;
    if (s_ui.apply_setpoint) {
        s_ui.apply_setpoint(clamped);
    }
    notifyStateChange();
}

void handleRemoteMode(uint8_t mode)
{
    if (mode >= MODE_COUNT) {
        ESP_LOGW(TAG, "peer: unknown mode %u -- ignored", (unsigned)mode);
        return;
    }
    ESP_LOGI(TAG, "peer: mode set to %s", modeName(mode));
    s_state.mode = mode;
    if (s_ui.apply_mode) {
        s_ui.apply_mode(mode);
    }
    notifyStateChange();
}

void handleRemoteToggles(uint8_t bitfield)
{
    ESP_LOGI(TAG, "peer: toggle bitfield 0x%02x", bitfield);
    for (uint8_t i = 0; i < TOGGLE_COUNT; i++) {
        bool state = (bitfield & (1u << i)) != 0;
        if (state == s_state.toggles[i]) {
            continue; // only move the widgets that actually changed
        }
        s_state.toggles[i] = state;
        if (s_ui.apply_toggle) {
            s_ui.apply_toggle(i, state);
        }
    }
    notifyStateChange();
}

void handleRemotePulse(uint8_t button_id)
{
    ESP_LOGI(TAG, "peer: pulse from button %u", (unsigned)button_id);
    s_state.pulse_count++;
    if (s_ui.apply_pulse) {
        s_ui.apply_pulse(s_state.pulse_count);
    }
    notifyStateChange();
}

/** True when the on-screen widget still needs to be moved for a local
 *  action. A touchscreen gesture has already moved its own widget; a web
 *  command has not (the display must follow it, per the "web GUI is an
 *  extension of the native display" requirement). */
bool needsUiApply(Source source)
{
    return source == Source::Web;
}

} // namespace

const char *toggleName(uint8_t id)
{
    switch (id) {
    case TOGGLE_SAMPLE: return "Sample";
    case TOGGLE_PUMP:   return "Pump";
    case TOGGLE_VALVE:  return "Valve";
    case TOGGLE_LIGHT:  return "Light";
    default:            return "?";
    }
}

const char *modeName(uint8_t mode)
{
    switch (mode) {
    case MODE_IDLE:    return "Idle";
    case MODE_MANUAL:  return "Manual";
    case MODE_AUTO:    return "Auto";
    case MODE_SERVICE: return "Service";
    default:           return "?";
    }
}

void init()
{
    comm_protocol::onDialReceived(handleRemoteDial);
    comm_protocol::onTogglesReceived(handleRemoteToggles);
    comm_protocol::onSpeedReceived(handleRemoteSpeed);
    comm_protocol::onModeReceived(handleRemoteMode);
    comm_protocol::onSetpointReceived(handleRemoteSetpoint);
    comm_protocol::onPulseReceived(handleRemotePulse);
}

void setUiCallbacks(const UiCallbacks &callbacks)
{
    s_ui = callbacks;
}

void clearUiCallbacks()
{
    s_ui = UiCallbacks{};
}

void onStateChange(StateChangeCallback cb)
{
    s_state_change_cbs.push_back(std::move(cb));
}

void setDial(uint8_t value, Source source)
{
    s_state.dial_value = value > DIAL_MAX ? DIAL_MAX : value;
    if (needsUiApply(source) && s_ui.apply_dial) {
        s_ui.apply_dial(s_state.dial_value);
    }
    comm_protocol::sendDial(s_state.dial_value);
    notifyStateChange();
}

void setSpeed(uint16_t value, Source source)
{
    s_state.speed = value > SPEED_MAX ? SPEED_MAX : value;
    if (needsUiApply(source) && s_ui.apply_speed) {
        s_ui.apply_speed(s_state.speed);
    }
    comm_protocol::sendSpeed(s_state.speed);
    notifyStateChange();
}

void setSetpoint(int16_t value, Source source)
{
    s_state.setpoint = std::max(SETPOINT_MIN, std::min(SETPOINT_MAX, value));
    if (needsUiApply(source) && s_ui.apply_setpoint) {
        s_ui.apply_setpoint(s_state.setpoint);
    }
    comm_protocol::sendSetpoint(s_state.setpoint);
    notifyStateChange();
}

void setMode(uint8_t mode, Source source)
{
    if (mode >= MODE_COUNT) {
        return; // out-of-range selection from the web: ignore rather than trust
    }
    s_state.mode = mode;
    if (needsUiApply(source) && s_ui.apply_mode) {
        s_ui.apply_mode(mode);
    }
    comm_protocol::sendMode(mode);
    notifyStateChange();
}

void setToggle(uint8_t id, bool state, Source source)
{
    if (id >= TOGGLE_COUNT) {
        return;
    }
    s_state.toggles[id] = state;
    if (needsUiApply(source) && s_ui.apply_toggle) {
        s_ui.apply_toggle(id, state);
    }
    // The whole bank travels in one bitfield, so every toggle change sends
    // the current state of all of them -- self-healing if a frame is lost.
    comm_protocol::sendToggles(toggleBitfield());
    notifyStateChange();
}

void firePulse(Source source)
{
    // A momentary event, not a held state: both boards just count it. The
    // local indicator flashes for a web press too (same as any other web
    // action moving the on-screen widgets); a touchscreen press is already
    // flashed by the button's own press feedback plus this call.
    (void)source;
    s_state.pulse_count++;
    if (s_ui.apply_pulse) {
        s_ui.apply_pulse(s_state.pulse_count);
    }
    comm_protocol::sendPulse(PULSE_BUTTON_MAIN);
    notifyStateChange();
}

State current()
{
    return s_state;
}

} // namespace machine_state
