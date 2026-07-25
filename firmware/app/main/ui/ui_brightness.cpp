#include "ui_brightness.h"

#include "display_settings.h"

namespace ui_brightness {
namespace {

lv_obj_t *s_dropdown = nullptr;
lv_obj_t *s_slider = nullptr;
lv_obj_t *s_value_label = nullptr;

void updateValueLabel(int32_t percent)
{
    lv_label_set_text_fmt(s_value_label, "%d%%", (int)percent);
}

/**
 * The panel is updated live while dragging (that is the whole point of a
 * brightness slider -- you aim at what looks right), but NVS is only
 * written on release, so a single adjustment costs one flash write instead
 * of one per pixel of travel.
 */
void sliderValueChangedCb(lv_event_t *e)
{
    int32_t value = lv_slider_get_value(s_slider);
    updateValueLabel(value);
    display_settings::setBrightness(static_cast<uint8_t>(value), /*persist=*/false);
}

void sliderReleasedCb(lv_event_t *e)
{
    display_settings::setBrightness(static_cast<uint8_t>(lv_slider_get_value(s_slider)),
                                    /*persist=*/true);
}

} // namespace

void init(lv_obj_t *screen)
{
    s_dropdown = lv_obj_create(screen);
    lv_obj_set_size(s_dropdown, 300, LV_SIZE_CONTENT);
    lv_obj_align(s_dropdown, LV_ALIGN_TOP_RIGHT, -8, 64);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_dropdown, 12, 0);
    lv_obj_set_style_clip_corner(s_dropdown, true, 0);
    lv_obj_set_style_bg_color(s_dropdown, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(s_dropdown, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_dropdown, 1, 0);
    lv_obj_set_style_pad_all(s_dropdown, 16, 0);
    lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_dropdown, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dropdown, 10, 0);

    lv_obj_t *title = lv_label_create(s_dropdown);
    lv_label_set_text(title, "Display");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *row = lv_obj_create(s_dropdown);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption = lv_label_create(row);
    lv_label_set_text(caption, LV_SYMBOL_EYE_OPEN "  Brightness");
    lv_obj_set_style_text_color(caption, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 0, 0);

    s_value_label = lv_label_create(row);
    lv_obj_set_style_text_color(s_value_label, lv_color_white(), 0);
    lv_obj_align(s_value_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_slider = lv_slider_create(row);
    lv_obj_set_width(s_slider, LV_PCT(100));
    lv_slider_set_range(s_slider, display_settings::BRIGHTNESS_MIN,
                        display_settings::BRIGHTNESS_MAX);
    lv_obj_align(s_slider, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_add_event_cb(s_slider, sliderValueChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_slider, sliderReleasedCb, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *hint = lv_label_create(s_dropdown);
    lv_label_set_text(hint, "Local to this screen -- not shared with the peer board.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6e7681), 0);

    uint8_t now = display_settings::brightness();
    lv_slider_set_value(s_slider, now, LV_ANIM_OFF);
    updateValueLabel(now);
}

void toggleDropdown(lv_obj_t *anchor)
{
    (void)anchor;
    if (lv_obj_has_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN)) {
        // Re-sync in case something else changed the brightness meanwhile.
        uint8_t now = display_settings::brightness();
        lv_slider_set_value(s_slider, now, LV_ANIM_OFF);
        updateValueLabel(now);
        lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dropdown);
    } else {
        lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
}

void hideDropdown()
{
    if (s_dropdown != nullptr) {
        lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace ui_brightness
