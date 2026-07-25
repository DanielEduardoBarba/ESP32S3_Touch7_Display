#include "ui_machine.h"

#include <cstdint>

#include "lvgl_v8_port.h"
#include "machine_state.h"

namespace ui_machine {
namespace {

// Six equally sized cards laid out as a flex-wrapped grid inside the
// scene's content area (1024x600 panel, minus the header and padding).
constexpr lv_coord_t CARD_W = 296;
constexpr lv_coord_t CARD_H = 205;
constexpr lv_coord_t CARD_GAP = 16;

lv_obj_t *s_content = nullptr;
lv_obj_t *s_dial = nullptr;
lv_obj_t *s_dial_value_label = nullptr;
lv_obj_t *s_speed = nullptr;
lv_obj_t *s_speed_value_label = nullptr;
lv_obj_t *s_setpoint_value_label = nullptr;
lv_obj_t *s_mode = nullptr;
lv_obj_t *s_toggles[machine_state::TOGGLE_COUNT] = {};
lv_obj_t *s_toggle_state_labels[machine_state::TOGGLE_COUNT] = {};
lv_obj_t *s_pulse_led = nullptr;
lv_obj_t *s_pulse_count_label = nullptr;

// --- Small helpers ---------------------------------------------------------

void updateDialLabel(int32_t value)
{
    lv_label_set_text_fmt(s_dial_value_label, "%d", (int)value);
}

void updateSpeedLabel(int32_t value)
{
    lv_label_set_text_fmt(s_speed_value_label, "%d", (int)value);
}

void updateSetpointLabel(int32_t value)
{
    lv_label_set_text_fmt(s_setpoint_value_label, "%d", (int)value);
}

void updateToggleLabel(uint8_t id, bool state)
{
    lv_label_set_text(s_toggle_state_labels[id], state ? "ON" : "OFF");
}

void updatePulseCountLabel(uint32_t count)
{
    lv_label_set_text_fmt(s_pulse_count_label, "%u pulses", (unsigned)count);
}

/** One card in the grid: rounded panel with a grey caption at the top. */
lv_obj_t *makeCard(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption = lv_label_create(card);
    lv_label_set_text(caption, title);
    lv_obj_set_style_text_color(caption, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

/** Flashes the pulse indicator. The animation is bound to the LED object,
 *  so LVGL cancels it automatically if the scene is destroyed mid-flash. */
void flashPulseLed()
{
    lv_led_on(s_pulse_led);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_pulse_led);
    lv_anim_set_exec_cb(&anim, [](void *obj, int32_t value) {
        lv_led_set_brightness(static_cast<lv_obj_t *>(obj), static_cast<uint8_t>(value));
    });
    lv_anim_set_values(&anim, LV_LED_BRIGHT_MAX, LV_LED_BRIGHT_MIN);
    lv_anim_set_time(&anim, 450);
    lv_anim_start(&anim);
}

// --- Local (touchscreen) input ---------------------------------------------

/**
 * The dial and the slider fire LV_EVENT_VALUE_CHANGED continuously while
 * being dragged (used here only to update the live label), and
 * LV_EVENT_RELEASED once when the user lets go -- that's the moment we
 * actually transmit, so dragging a control around doesn't flood the RS485
 * bus/WebSocket with a packet per pixel of movement.
 */
void dialValueChangedCb(lv_event_t *e)
{
    updateDialLabel(lv_arc_get_value(s_dial));
}

void dialReleasedCb(lv_event_t *e)
{
    machine_state::setDial(static_cast<uint8_t>(lv_arc_get_value(s_dial)),
                           machine_state::Source::LocalUi);
}

void speedValueChangedCb(lv_event_t *e)
{
    updateSpeedLabel(lv_slider_get_value(s_speed));
}

void speedReleasedCb(lv_event_t *e)
{
    machine_state::setSpeed(static_cast<uint16_t>(lv_slider_get_value(s_speed)),
                            machine_state::Source::LocalUi);
}

/** The -/+ buttons step the setpoint (which can go negative). machine_state
 *  clamps to the allowed range, so the label is refreshed from the result
 *  rather than from our own arithmetic. */
void setpointStepCb(lv_event_t *e)
{
    int step = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    int wanted = machine_state::current().setpoint + step;
    machine_state::setSetpoint(static_cast<int16_t>(wanted), machine_state::Source::LocalUi);
    updateSetpointLabel(machine_state::current().setpoint);
}

/** Discrete controls (dropdown, switches, buttons) are single actions, not
 *  drags, so they transmit on every change rather than waiting for a
 *  separate "release" event. */
void modeValueChangedCb(lv_event_t *e)
{
    machine_state::setMode(static_cast<uint8_t>(lv_dropdown_get_selected(s_mode)),
                           machine_state::Source::LocalUi);
}

void toggleValueChangedCb(lv_event_t *e)
{
    uint8_t id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    bool state = lv_obj_has_state(s_toggles[id], LV_STATE_CHECKED);
    updateToggleLabel(id, state);
    machine_state::setToggle(id, state, machine_state::Source::LocalUi);
}

void pulseClickedCb(lv_event_t *e)
{
    // machine_state calls back into applyPulseFromElsewhere() for a local
    // press too, which is what flashes the LED and bumps the counter here.
    machine_state::firePulse(machine_state::Source::LocalUi);
}

// --- Applying changes that came from somewhere else ------------------------

/**
 * Called by machine_state whenever a value changes for a reason OTHER than
 * this board's own touchscreen (i.e. an RS485 packet from the other board,
 * or a command from a web browser). Runs on whichever task received that
 * event (not the LVGL task), so these lock before touching any LVGL object
 * -- the LVGL mutex is recursive, so it is also safe when a local UI event
 * callback (already on the LVGL task) leads here.
 *
 * They all use the "set value" API rather than simulating user input:
 * lv_arc_set_value()/lv_slider_set_value()/lv_dropdown_set_selected() and
 * adding/clearing LV_STATE_CHECKED intentionally do NOT fire
 * LV_EVENT_VALUE_CHANGED/RELEASED, so applying a remote update here can
 * never accidentally trigger the handlers above and re-transmit the value
 * we just received (which is what would cause an infinite echo loop
 * between two connected boards).
 */
void applyDialFromElsewhere(uint8_t value)
{
    lvgl_port_lock(-1);
    lv_arc_set_value(s_dial, value);
    updateDialLabel(value);
    lvgl_port_unlock();
}

void applySpeedFromElsewhere(uint16_t value)
{
    lvgl_port_lock(-1);
    lv_slider_set_value(s_speed, value, LV_ANIM_OFF);
    updateSpeedLabel(value);
    lvgl_port_unlock();
}

void applySetpointFromElsewhere(int16_t value)
{
    lvgl_port_lock(-1);
    updateSetpointLabel(value);
    lvgl_port_unlock();
}

void applyModeFromElsewhere(uint8_t mode)
{
    lvgl_port_lock(-1);
    lv_dropdown_set_selected(s_mode, mode);
    lvgl_port_unlock();
}

void applyToggleFromElsewhere(uint8_t id, bool state)
{
    if (id >= machine_state::TOGGLE_COUNT) {
        return;
    }
    lvgl_port_lock(-1);
    if (state) {
        lv_obj_add_state(s_toggles[id], LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_toggles[id], LV_STATE_CHECKED);
    }
    updateToggleLabel(id, state);
    lvgl_port_unlock();
}

void applyPulseFromElsewhere(uint32_t count)
{
    lvgl_port_lock(-1);
    updatePulseCountLabel(count);
    flashPulseLed();
    lvgl_port_unlock();
}

// --- Card builders ---------------------------------------------------------

void buildDialCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Dial (0-100)");

    s_dial = lv_arc_create(card);
    lv_obj_set_size(s_dial, 128, 128);
    lv_arc_set_range(s_dial, 0, machine_state::DIAL_MAX);
    lv_obj_align(s_dial, LV_ALIGN_CENTER, 0, 12);
    lv_obj_add_event_cb(s_dial, dialValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dial, dialReleasedCb, LV_EVENT_RELEASED, nullptr);

    s_dial_value_label = lv_label_create(s_dial);
    lv_obj_set_style_text_font(s_dial_value_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_dial_value_label, lv_color_white(), 0);
    lv_obj_center(s_dial_value_label);
}

void buildSpeedCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Speed (0-1000)");

    s_speed_value_label = lv_label_create(card);
    lv_obj_set_style_text_font(s_speed_value_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_speed_value_label, lv_color_white(), 0);
    lv_obj_align(s_speed_value_label, LV_ALIGN_TOP_RIGHT, 0, -4);

    s_speed = lv_slider_create(card);
    lv_obj_set_width(s_speed, 248);
    lv_slider_set_range(s_speed, 0, machine_state::SPEED_MAX);
    lv_obj_align(s_speed, LV_ALIGN_CENTER, 0, 16);
    lv_obj_add_event_cb(s_speed, speedValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_speed, speedReleasedCb, LV_EVENT_RELEASED, nullptr);
}

void buildSetpointCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Setpoint (-50..150)");

    auto make_step_button = [&](const char *text, int step, lv_align_t align) {
        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, 64, 56);
        lv_obj_align(btn, align, 0, 16);
        lv_obj_add_event_cb(btn, setpointStepCb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(step)));
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_center(label);
    };
    make_step_button(LV_SYMBOL_MINUS, -machine_state::SETPOINT_STEP, LV_ALIGN_LEFT_MID);
    make_step_button(LV_SYMBOL_PLUS, machine_state::SETPOINT_STEP, LV_ALIGN_RIGHT_MID);

    s_setpoint_value_label = lv_label_create(card);
    lv_obj_set_style_text_font(s_setpoint_value_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_setpoint_value_label, lv_color_white(), 0);
    lv_obj_align(s_setpoint_value_label, LV_ALIGN_CENTER, 0, 16);
}

void buildModeCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Mode");

    s_mode = lv_dropdown_create(card);
    lv_dropdown_set_options_static(s_mode, "Idle\nManual\nAuto\nService");
    lv_obj_set_width(s_mode, 248);
    lv_obj_align(s_mode, LV_ALIGN_CENTER, 0, 12);
    lv_obj_add_event_cb(s_mode, modeValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void buildTogglesCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Toggles");

    // 2x2 grid of switches, each with its name above and ON/OFF beside it.
    // All four ride in ONE bitfield on the wire (see comm_protocol.h).
    constexpr lv_coord_t COL_X[2] = {0, 140};
    constexpr lv_coord_t ROW_Y[2] = {34, 108};
    for (uint8_t id = 0; id < machine_state::TOGGLE_COUNT; id++) {
        lv_coord_t x = COL_X[id % 2];
        lv_coord_t y = ROW_Y[id / 2];

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, machine_state::toggleName(id));
        lv_obj_set_style_text_color(name, lv_color_hex(0xe6edf3), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, x, y);

        s_toggles[id] = lv_switch_create(card);
        lv_obj_set_size(s_toggles[id], 52, 26);
        lv_obj_align(s_toggles[id], LV_ALIGN_TOP_LEFT, x, y + 24);
        lv_obj_add_event_cb(s_toggles[id], toggleValueChangedCb, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(id)));

        s_toggle_state_labels[id] = lv_label_create(card);
        lv_obj_set_style_text_color(s_toggle_state_labels[id], lv_color_hex(0x9aa4b2), 0);
        lv_obj_align(s_toggle_state_labels[id], LV_ALIGN_TOP_LEFT, x + 60, y + 28);
    }
}

void buildPulseCard(lv_obj_t *parent)
{
    lv_obj_t *card = makeCard(parent, "Pulse (momentary)");

    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, 150, 60);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 10);
    lv_obj_add_event_cb(btn, pulseClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Send pulse");
    lv_obj_center(btn_label);

    s_pulse_led = lv_led_create(card);
    lv_obj_set_size(s_pulse_led, 34, 34);
    lv_obj_align(s_pulse_led, LV_ALIGN_RIGHT_MID, -20, 10);
    lv_led_set_color(s_pulse_led, lv_color_hex(0x3fb950));
    lv_led_set_brightness(s_pulse_led, LV_LED_BRIGHT_MIN);

    s_pulse_count_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_pulse_count_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(s_pulse_count_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

/** Rebuilds start from the CURRENT machine state: this scene is destroyed
 *  while other scenes are shown, so the widgets are new but the data (and
 *  anything the peer changed meanwhile) outlives them. */
void loadCurrentState()
{
    machine_state::State now = machine_state::current();

    lv_arc_set_value(s_dial, now.dial_value);
    updateDialLabel(now.dial_value);

    lv_slider_set_value(s_speed, now.speed, LV_ANIM_OFF);
    updateSpeedLabel(now.speed);

    updateSetpointLabel(now.setpoint);
    lv_dropdown_set_selected(s_mode, now.mode);

    for (uint8_t id = 0; id < machine_state::TOGGLE_COUNT; id++) {
        if (now.toggles[id]) {
            lv_obj_add_state(s_toggles[id], LV_STATE_CHECKED);
        }
        updateToggleLabel(id, now.toggles[id]);
    }

    updatePulseCountLabel(now.pulse_count);
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
    lv_label_set_text(hint, "Every control here is synced live over RS485 with another board, and with the web UI.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    // Flex-wrapped card grid: each card keeps its own size and simply flows
    // onto the next row, so adding a seventh control needs no re-layout.
    lv_obj_t *cards = lv_obj_create(s_content);
    lv_obj_set_size(cards, LV_PCT(100), LV_VER_RES - header_height - 48 - 62);
    lv_obj_align_to(cards, hint, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_set_style_pad_row(cards, CARD_GAP, 0);
    lv_obj_set_style_pad_column(cards, CARD_GAP, 0);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW_WRAP);

    buildDialCard(cards);
    buildSpeedCard(cards);
    buildSetpointCard(cards);
    buildModeCard(cards);
    buildTogglesCard(cards);
    buildPulseCard(cards);

    loadCurrentState();

    // Register the callbacks machine_state uses to move these widgets when
    // the state changes for a reason other than this board's own touchscreen.
    machine_state::UiCallbacks callbacks;
    callbacks.apply_dial = applyDialFromElsewhere;
    callbacks.apply_speed = applySpeedFromElsewhere;
    callbacks.apply_setpoint = applySetpointFromElsewhere;
    callbacks.apply_mode = applyModeFromElsewhere;
    callbacks.apply_toggle = applyToggleFromElsewhere;
    callbacks.apply_pulse = applyPulseFromElsewhere;
    machine_state::setUiCallbacks(callbacks);

    return s_content;
}

void destroy()
{
    // Detach machine_state's UI hooks FIRST so a peer/web event arriving
    // mid-teardown can't touch dying widgets.
    machine_state::clearUiCallbacks();
    if (s_content != nullptr) {
        lv_obj_del(s_content);
    }
    s_content = nullptr;
    s_dial = nullptr;
    s_dial_value_label = nullptr;
    s_speed = nullptr;
    s_speed_value_label = nullptr;
    s_setpoint_value_label = nullptr;
    s_mode = nullptr;
    for (uint8_t id = 0; id < machine_state::TOGGLE_COUNT; id++) {
        s_toggles[id] = nullptr;
        s_toggle_state_labels[id] = nullptr;
    }
    s_pulse_led = nullptr;
    s_pulse_count_label = nullptr;
}

} // namespace ui_machine
