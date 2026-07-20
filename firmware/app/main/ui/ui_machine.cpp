#include "ui_machine.h"

#include "lvgl_v8_port.h"
#include "machine_state.h"

namespace ui_machine {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_dial = nullptr;
lv_obj_t *s_dial_value_label = nullptr;
lv_obj_t *s_toggle = nullptr;
lv_obj_t *s_toggle_state_label = nullptr;

void updateDialLabel(int32_t value)
{
    lv_label_set_text_fmt(s_dial_value_label, "%d", (int)value);
}

void updateToggleLabel(bool state)
{
    lv_label_set_text(s_toggle_state_label, state ? "ON" : "OFF");
}

/**
 * The dial fires LV_EVENT_VALUE_CHANGED continuously while being dragged
 * (used here only to update the live label), and LV_EVENT_RELEASED once
 * when the user lets go -- that's the moment we actually transmit, so
 * dragging the dial around doesn't flood the RS485 bus/WebSocket with a
 * packet per pixel of movement.
 */
void dialValueChangedCb(lv_event_t *e)
{
    updateDialLabel(lv_arc_get_value(s_dial));
}

void dialReleasedCb(lv_event_t *e)
{
    uint8_t value = static_cast<uint8_t>(lv_arc_get_value(s_dial));
    machine_state::setDialFromLocalUi(value);
}

/** The toggle is a discrete on/off action (not a drag), so we transmit on
 *  every change rather than waiting for a separate "release" event. */
void toggleValueChangedCb(lv_event_t *e)
{
    bool state = lv_obj_has_state(s_toggle, LV_STATE_CHECKED);
    updateToggleLabel(state);
    machine_state::setToggleFromLocalUi(state);
}

/**
 * Called by machine_state whenever the dial value changes for a reason
 * OTHER than this board's own touchscreen (i.e. an RS485 packet from the
 * other board, or a command from a web browser). Runs on whichever task
 * received that event (not the LVGL task), so it must lock before touching
 * any LVGL objects.
 *
 * Uses lv_arc_set_value() rather than simulating a drag: that function
 * intentionally does NOT fire LV_EVENT_VALUE_CHANGED/RELEASED, so applying a
 * remote update here can never accidentally trigger dialReleasedCb() above
 * and re-transmit the value we just received (which is what would cause an
 * infinite echo loop between two connected boards).
 */
void applyDialFromElsewhere(uint8_t value)
{
    lvgl_port_lock(-1);
    lv_arc_set_value(s_dial, value);
    updateDialLabel(value);
    lvgl_port_unlock();
}

/** Same idea as applyDialFromElsewhere(), but for the toggle: directly
 *  adding/clearing LV_STATE_CHECKED does not fire LV_EVENT_VALUE_CHANGED,
 *  so it can't cause a re-transmit loop either. */
void applyToggleFromElsewhere(bool state)
{
    lvgl_port_lock(-1);
    if (state) {
        lv_obj_add_state(s_toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_toggle, LV_STATE_CHECKED);
    }
    updateToggleLabel(state);
    lvgl_port_unlock();
}

} // namespace

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height)
{
    s_content = lv_obj_create(screen);
    lv_obj_set_size(s_content, LV_PCT(100), LV_VER_RES - header_height);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, header_height);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(0x101317), 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 24, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_content);
    lv_label_set_text(title, "Machine");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(s_content);
    lv_label_set_text(hint, "Synced live over RS485 with another board, and with the web UI.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    // --- Dial card -----------------------------------------------------
    lv_obj_t *dial_card = lv_obj_create(s_content);
    lv_obj_set_size(dial_card, 300, 300);
    lv_obj_align_to(dial_card, hint, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    lv_obj_set_style_radius(dial_card, 12, 0);
    lv_obj_set_style_bg_color(dial_card, lv_color_hex(0x1c2128), 0);
    lv_obj_clear_flag(dial_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dial_title = lv_label_create(dial_card);
    lv_label_set_text(dial_title, "Dial (0-100)");
    lv_obj_set_style_text_color(dial_title, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(dial_title, LV_ALIGN_TOP_MID, 0, 12);

    s_dial = lv_arc_create(dial_card);
    lv_obj_set_size(s_dial, 200, 200);
    lv_arc_set_range(s_dial, 0, 100);
    lv_arc_set_value(s_dial, 0);
    lv_obj_align(s_dial, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_event_cb(s_dial, dialValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dial, dialReleasedCb, LV_EVENT_RELEASED, nullptr);

    s_dial_value_label = lv_label_create(s_dial);
    lv_label_set_text(s_dial_value_label, "0");
    lv_obj_set_style_text_font(s_dial_value_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_dial_value_label, lv_color_white(), 0);
    lv_obj_center(s_dial_value_label);

    // --- Toggle card -----------------------------------------------------
    lv_obj_t *toggle_card = lv_obj_create(s_content);
    lv_obj_set_size(toggle_card, 260, 120);
    lv_obj_align_to(toggle_card, dial_card, LV_ALIGN_OUT_RIGHT_TOP, 24, 0);
    lv_obj_set_style_radius(toggle_card, 12, 0);
    lv_obj_set_style_bg_color(toggle_card, lv_color_hex(0x1c2128), 0);
    lv_obj_clear_flag(toggle_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *toggle_title = lv_label_create(toggle_card);
    lv_label_set_text(toggle_title, "Sample toggle");
    lv_obj_set_style_text_color(toggle_title, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(toggle_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_toggle = lv_switch_create(toggle_card);
    lv_obj_align_to(s_toggle, toggle_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_add_event_cb(s_toggle, toggleValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_toggle_state_label = lv_label_create(toggle_card);
    lv_label_set_text(s_toggle_state_label, "OFF");
    lv_obj_set_style_text_color(s_toggle_state_label, lv_color_white(), 0);
    lv_obj_align_to(s_toggle_state_label, s_toggle, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    // Register the callbacks machine_state uses to move these widgets when
    // the state changes for a reason other than this board's own touchscreen.
    machine_state::setUiCallbacks(applyDialFromElsewhere, applyToggleFromElsewhere);

    // Rebuilds start from the CURRENT machine state (this scene is
    // destroyed while other scenes are shown; the data outlives the widgets).
    machine_state::State now = machine_state::current();
    lv_arc_set_value(s_dial, now.dial_value);
    updateDialLabel(now.dial_value);
    if (now.toggle_state) {
        lv_obj_add_state(s_toggle, LV_STATE_CHECKED);
    }
    updateToggleLabel(now.toggle_state);

    return s_content;
}

void destroy()
{
    // Detach machine_state's UI hooks FIRST so a peer/web event arriving
    // mid-teardown can't touch dying widgets.
    machine_state::setUiCallbacks(nullptr, nullptr);
    if (s_content != nullptr) {
        lv_obj_del(s_content);
    }
    s_content = nullptr;
    s_dial = nullptr;
    s_dial_value_label = nullptr;
    s_toggle = nullptr;
    s_toggle_state_label = nullptr;
}

} // namespace ui_machine
