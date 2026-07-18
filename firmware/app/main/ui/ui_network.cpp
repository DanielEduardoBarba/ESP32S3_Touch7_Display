#include "ui_network.h"

#include "lvgl_v8_port.h"
#include "wifi_manager.h"

namespace ui_network {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_wifi_status_label = nullptr;

void onWifiStateChange(wifi_manager::State state, const std::string &ssid, const std::string &ip)
{
    lvgl_port_lock(-1);
    if (s_wifi_status_label != nullptr) {
        switch (state) {
        case wifi_manager::State::Connecting:
            lv_label_set_text_fmt(s_wifi_status_label, "WiFi: connecting to %s...", ssid.c_str());
            break;
        case wifi_manager::State::Connected:
            lv_label_set_text_fmt(s_wifi_status_label, "WiFi: connected to %s (%s)", ssid.c_str(), ip.c_str());
            break;
        case wifi_manager::State::Failed:
            lv_label_set_text(s_wifi_status_label, "WiFi: connection failed");
            break;
        default:
            lv_label_set_text(s_wifi_status_label, "WiFi: not connected");
            break;
        }
    }
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
    lv_label_set_text(title, "Network");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *card = lv_obj_create(s_content);
    lv_obj_set_size(card, 460, 140);
    lv_obj_align_to(card, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card_title = lv_label_create(card);
    lv_label_set_text(card_title, "Device status");
    lv_obj_set_style_text_color(card_title, lv_color_hex(0x9aa4b2), 0);

    s_wifi_status_label = lv_label_create(card);
    lv_label_set_text(s_wifi_status_label, "WiFi: not connected");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_white(), 0);
    lv_obj_set_width(s_wifi_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_wifi_status_label, card_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    lv_obj_t *hint = lv_label_create(s_content);
    lv_label_set_text(hint, "Use the WiFi icon (top-right) to connect this device to a network.\n"
                            "The on-device web UI is served on port 80 once connected.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4b2), 0);
    lv_obj_set_width(hint, 460);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(hint, card, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);

    wifi_manager::onStateChange(onWifiStateChange);

    return s_content;
}

} // namespace ui_network
