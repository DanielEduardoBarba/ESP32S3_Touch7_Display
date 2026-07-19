#include "machine_state.h"

#include <vector>

#include "esp_log.h"
#include "comm_protocol.h"

namespace machine_state {
namespace {

const char *TAG = "machine_state";

State s_state;

ApplyDialCallback s_apply_dial;
ApplyToggleCallback s_apply_toggle;
std::vector<StateChangeCallback> s_state_change_cbs;

/** Packs all on/off devices into the compact status bitfield (bit 0 =
 *  the sample toggle; future toggles get bits 1..7). */
uint8_t toggleBitfield()
{
    return s_state.toggle_state ? 0x01 : 0x00;
}

void notifyStateChange()
{
    for (const auto &cb : s_state_change_cbs) {
        cb(s_state);
    }
}

/** A frame arrived from the other board: move the local widgets to match,
 *  but do NOT transmit here -- doing so would immediately bounce the value
 *  back to the sender, forever. Only *local* actions (touchscreen or web)
 *  transmit. */
void handleRemoteDial(uint8_t value)
{
    ESP_LOGI(TAG, "peer: dial set to %d", value);
    s_state.dial_value = value;
    if (s_apply_dial) {
        s_apply_dial(value);
    }
    notifyStateChange();
}

void handleRemoteToggles(uint8_t bitfield)
{
    bool state = (bitfield & 0x01) != 0;
    ESP_LOGI(TAG, "peer: toggle bitfield 0x%02x (sample=%d)", bitfield, state);
    s_state.toggle_state = state;
    if (s_apply_toggle) {
        s_apply_toggle(state);
    }
    notifyStateChange();
}

} // namespace

void init()
{
    comm_protocol::onDialReceived(handleRemoteDial);
    comm_protocol::onTogglesReceived(handleRemoteToggles);
}

void setUiCallbacks(ApplyDialCallback apply_dial, ApplyToggleCallback apply_toggle)
{
    s_apply_dial = std::move(apply_dial);
    s_apply_toggle = std::move(apply_toggle);
}

void onStateChange(StateChangeCallback cb)
{
    s_state_change_cbs.push_back(std::move(cb));
}

void setDialFromLocalUi(uint8_t value)
{
    // The touchscreen widget already shows this value (the user just
    // dragged it there), so there's no need to call s_apply_dial here --
    // only the *other* outputs (peer link + web) need to be told about it.
    s_state.dial_value = value;
    comm_protocol::sendDial(value);
    notifyStateChange();
}

void setToggleFromLocalUi(bool state)
{
    s_state.toggle_state = state;
    comm_protocol::sendToggles(toggleBitfield());
    notifyStateChange();
}

void setDialFromWeb(uint8_t value)
{
    // Unlike the local-UI case, the touchscreen widget does NOT already
    // reflect this value -- it needs to be moved too, exactly as if a
    // finger had dragged it, per the "web GUI is an extension of the
    // native display" requirement.
    s_state.dial_value = value;
    if (s_apply_dial) {
        s_apply_dial(value);
    }
    comm_protocol::sendDial(value);
    notifyStateChange();
}

void setToggleFromWeb(bool state)
{
    s_state.toggle_state = state;
    if (s_apply_toggle) {
        s_apply_toggle(state);
    }
    comm_protocol::sendToggles(toggleBitfield());
    notifyStateChange();
}

State current()
{
    return s_state;
}

} // namespace machine_state
