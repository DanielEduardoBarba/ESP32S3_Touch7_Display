#include "ui_debug.h"

#include <string>

#include "app_config.h"
#include "log_store.h"

namespace ui_debug {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_panel = nullptr;
lv_obj_t *s_label = nullptr;
lv_timer_t *s_refresh_timer = nullptr;
uint32_t s_last_rev = 0;

/** Runs on the LVGL task; only rebuilds the (potentially large) text when
 *  the scene is visible AND new lines actually arrived. */
void refreshTimerCb(lv_timer_t *)
{
    if (s_content == nullptr || lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    uint32_t rev = log_store::revision();
    if (rev == s_last_rev) {
        return;
    }
    s_last_rev = rev;

    auto lines = log_store::snapshot();
    std::string text;
    text.reserve(lines.size() * 64);
    for (const auto &l : lines) {
        text += l;
        text += '\n';
    }
    lv_label_set_text(s_label, text.c_str());
    lv_obj_scroll_to_y(s_panel, LV_COORD_MAX, LV_ANIM_OFF); // follow the tail
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
    lv_label_set_text(title, "Debug");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(s_content);
    lv_label_set_text_fmt(hint, "Live device log (same lines as the serial console, last %d kept)",
                          APP_LOG_STORE_MAX);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    // Scrollable log panel filling the rest of the scene.
    s_panel = lv_obj_create(s_content);
    lv_obj_set_size(s_panel, LV_PCT(100), LV_VER_RES - header_height - 120);
    lv_obj_align_to(s_panel, hint, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lv_obj_set_style_radius(s_panel, 12, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x0b0e11), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_pad_all(s_panel, 12, 0);

    s_label = lv_label_create(s_panel);
    lv_label_set_text(s_label, "Waiting for log lines...");
    lv_obj_set_width(s_label, LV_PCT(100));
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x8fd18f), 0);

    s_refresh_timer = lv_timer_create(refreshTimerCb, APP_DEBUG_REFRESH_MS, nullptr);
    s_last_rev = 0; // force a repaint with whatever the ring holds now
    return s_content;
}

void destroy()
{
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    if (s_content != nullptr) {
        lv_obj_del(s_content);
    }
    s_content = nullptr;
    s_panel = nullptr;
    s_label = nullptr;
}

} // namespace ui_debug
