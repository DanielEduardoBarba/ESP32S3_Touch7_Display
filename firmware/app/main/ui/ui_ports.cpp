#include "ui_ports.h"

#include "../ports.h"

namespace ui_ports {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_active_label = nullptr;

void refreshActiveLabel()
{
    lv_label_set_text_fmt(s_active_label, "Active: %s", ports::name(ports::active()));
}

void transportBtnClickedCb(lv_event_t *e)
{
    auto transport = static_cast<ports::Transport>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (ports::setActive(transport)) {
        refreshActiveLabel();
    }
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
    lv_label_set_text(title, "Ports");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(s_content);
    lv_label_set_text(hint,
                       "Which link carries machine sync + firmware updates to the peer board.\n"
                       "Greyed-out entries exist in the codebase but are disabled in ports_config.h.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    // One row per transport; disabled ones are visible but not clickable,
    // making it obvious the capability exists and how to turn it on.
    lv_obj_t *list = lv_obj_create(s_content);
    lv_obj_set_size(list, 420, LV_SIZE_CONTENT);
    lv_obj_align_to(list, hint, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    lv_obj_set_style_radius(list, 12, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x1c2128), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (const auto &info : ports::transports()) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(info.enabled ? 0x272e37 : 0x1a1f26), 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_fmt(label, "%s%s", info.label, info.enabled ? "" : "  (disabled)");
        lv_obj_set_style_text_color(label, lv_color_hex(info.enabled ? 0xffffff : 0x555f6b), 0);
        lv_obj_center(label);

        if (info.enabled) {
            lv_obj_add_event_cb(btn, transportBtnClickedCb, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(
                                    static_cast<uint8_t>(info.id))));
        } else {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        }
    }

    s_active_label = lv_label_create(s_content);
    lv_obj_set_style_text_color(s_active_label, lv_color_hex(0x2ea043), 0);
    lv_obj_align_to(s_active_label, list, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    refreshActiveLabel();

    return s_content;
}

} // namespace ui_ports
