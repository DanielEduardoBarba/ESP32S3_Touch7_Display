#include "machine_state.h"

#include <vector>

#include "esp_log.h"
#include "rs485_protocol.h"

namespace machine_state {
namespace {

const char *TAG = "machine_state";

State s_state;

ApplyDialCallback s_apply_dial;
ApplyToggleCallback s_apply_toggle;
std::vector<StateChangeCallback> s_state_change_cbs;

void notifyStateChange()
{
    for (const auto &cb : s_state_change_cbs) {
        cb(s_state);
    }
}

/** RS485 frame arrived from the other board: move the local widgets to
 *  match, but do NOT call rs485_protocol::sendDial() here -- doing so would
 *  immediately bounce the value back to the sender, which would bounce it
 *  back again, forever. Only *local* actions (touchscreen or web) transmit. */
void handleRemoteDial(uint8_t value)
{
    ESP_LOGI(TAG, "RS485: dial set to %d", value);
    s_state.dial_value = value;
    if (s_apply_dial) {
        s_apply_dial(value);
    }
    notifyStateChange();
}

void handleRemoteToggle(uint8_t toggle_id, bool state)
{
    if (toggle_id != TOGGLE_ID_SAMPLE) {
        return; // not a toggle we know about (future-proofing for more toggles)
    }
    ESP_LOGI(TAG, "RS485: toggle %d set to %d", toggle_id, state);
    s_state.toggle_state = state;
    if (s_apply_toggle) {
        s_apply_toggle(state);
    }
    notifyStateChange();
}

} // namespace

void init()
{
    rs485_protocol::onDialReceived(handleRemoteDial);
    rs485_protocol::onToggleReceived(handleRemoteToggle);
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
    // only the *other* outputs (RS485 + web) need to be told about it.
    s_state.dial_value = value;
    rs485_protocol::sendDial(value);
    notifyStateChange();
}

void setToggleFromLocalUi(bool state)
{
    s_state.toggle_state = state;
    rs485_protocol::sendToggle(TOGGLE_ID_SAMPLE, state);
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
    rs485_protocol::sendDial(value);
    notifyStateChange();
}

void setToggleFromWeb(bool state)
{
    s_state.toggle_state = state;
    if (s_apply_toggle) {
        s_apply_toggle(state);
    }
    rs485_protocol::sendToggle(TOGGLE_ID_SAMPLE, state);
    notifyStateChange();
}

State current()
{
    return s_state;
}

} // namespace machine_state
