#include "ui_wifi.h"

#include <string>
#include <vector>

#include "lvgl_v8_port.h"
#include "ui_keyboard.h"
#include "wifi_manager.h"

namespace ui_wifi {
namespace {

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_dropdown = nullptr;
lv_obj_t *s_list = nullptr;
lv_obj_t *s_modal_bg = nullptr;
lv_obj_t *s_ssid_ta = nullptr;
lv_obj_t *s_pass_ta = nullptr;
lv_obj_t *s_modal_status = nullptr;
lv_obj_t *s_forget_btn = nullptr;

bool isConnectedTo(const std::string &ssid)
{
    return wifi_manager::state() == wifi_manager::State::Connected && wifi_manager::currentSsid() == ssid;
}

void openModal(const std::string &ssid)
{
    lv_textarea_set_text(s_ssid_ta, ssid.c_str());
    lv_textarea_set_text(s_pass_ta, "");
    lv_label_set_text(s_modal_status, "");
    if (isConnectedTo(ssid)) {
        lv_obj_clear_flag(s_forget_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_forget_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
}

void closeModal()
{
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN);
    ui_keyboard::hide();
}

void listItemClickedCb(lv_event_t *e)
{
    auto *ssid = static_cast<std::string *>(lv_event_get_user_data(e));
    if (ssid != nullptr) {
        openModal(*ssid);
    }
}

void clearList()
{
    // Free the heap-allocated SSID strings we stashed as user_data before
    // wiping the list's children.
    uint32_t count = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *btn = lv_obj_get_child(s_list, i);
        auto *ssid = static_cast<std::string *>(lv_obj_get_user_data(btn));
        delete ssid;
    }
    lv_obj_clean(s_list);
}

void onScanResults(const std::vector<wifi_manager::ApInfo> &results)
{
    lvgl_port_lock(-1);
    clearList();
    if (results.empty()) {
        lv_obj_t *empty_label = lv_label_create(s_list);
        lv_label_set_text(empty_label, "Scanning...");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x9aa4b2), 0);
    }
    for (const auto &ap : results) {
        std::string text = ap.ssid + (ap.secure ? "  " LV_SYMBOL_CLOSE : "");
        lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_WIFI, text.c_str());
        // lv_list buttons default to a white background with their own
        // rounding; blend them into the dark dropdown card instead (same
        // treatment as the hamburger menu, see ui_header.cpp).
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c2128), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x272e37), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xe6edf3), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        if (isConnectedTo(ap.ssid)) {
            // Highlight the currently-connected network so it's obvious at a
            // glance which one is active, without needing to open the modal.
            lv_obj_set_style_border_color(btn, lv_color_hex(0x2ea043), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        }
        auto *ssid_copy = new std::string(ap.ssid);
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, listItemClickedCb, LV_EVENT_CLICKED, ssid_copy);
    }
    lvgl_port_unlock();
}

void onWifiStateChange(wifi_manager::State state, const std::string &ssid, const std::string &ip)
{
    lvgl_port_lock(-1);
    if (s_modal_status != nullptr) {
        switch (state) {
        case wifi_manager::State::Connecting:
            lv_label_set_text_fmt(s_modal_status, "Connecting to %s...", ssid.c_str());
            break;
        case wifi_manager::State::Connected:
            lv_label_set_text_fmt(s_modal_status, "Connected! IP: %s", ip.c_str());
            break;
        case wifi_manager::State::Failed:
            lv_label_set_text(s_modal_status, "Connection failed. Check password and try again.");
            break;
        default:
            break;
        }
    }
    lvgl_port_unlock();
}

void connectBtnClickedCb(lv_event_t *e)
{
    const char *ssid = lv_textarea_get_text(s_ssid_ta);
    const char *password = lv_textarea_get_text(s_pass_ta);
    ui_keyboard::hide();
    lv_label_set_text(s_modal_status, "Connecting...");
    wifi_manager::connect(ssid, password);
}

void forgetBtnClickedCb(lv_event_t *e)
{
    wifi_manager::forget();
    lv_label_set_text(s_modal_status, "Forgotten. Disconnected.");
    lv_obj_add_flag(s_forget_btn, LV_OBJ_FLAG_HIDDEN);
}

void closeBtnClickedCb(lv_event_t *e)
{
    closeModal();
}

void textareaFocusedCb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    ui_keyboard::attach(ta);
}

void textareaDefocusedCb(lv_event_t *e)
{
    ui_keyboard::hide();
}

lv_obj_t *createLabeledTextarea(lv_obj_t *parent, const char *placeholder, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_add_event_cb(ta, textareaFocusedCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(ta, textareaDefocusedCb, LV_EVENT_DEFOCUSED, nullptr);
    return ta;
}

void buildDropdown(lv_obj_t *screen)
{
    s_dropdown = lv_obj_create(screen);
    lv_obj_set_size(s_dropdown, 300, 320);
    lv_obj_align(s_dropdown, LV_ALIGN_TOP_RIGHT, -8, 64);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_dropdown, 12, 0);
    lv_obj_set_style_clip_corner(s_dropdown, true, 0);
    lv_obj_set_style_bg_color(s_dropdown, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(s_dropdown, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_dropdown, 1, 0);
    lv_obj_set_style_pad_all(s_dropdown, 8, 0);
    lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_SCROLLABLE);
    // Stack title above the list so the list can't overlap/hide the title.
    lv_obj_set_flex_flow(s_dropdown, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dropdown, 6, 0);
    lv_obj_move_foreground(s_dropdown);

    lv_obj_t *title = lv_label_create(s_dropdown);
    lv_label_set_text(title, "Wi-Fi networks");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    s_list = lv_list_create(s_dropdown);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
}

void buildModal(lv_obj_t *screen)
{
    s_modal_bg = lv_obj_create(screen);
    lv_obj_remove_style_all(s_modal_bg);
    lv_obj_set_size(s_modal_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_bg, LV_OPA_50, 0);
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_modal_bg);

    lv_obj_t *card = lv_obj_create(s_modal_bg);
    lv_obj_set_size(card, 420, 320);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Connect to Wi-Fi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, closeBtnClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);

    lv_obj_t *ssid_label = lv_label_create(card);
    lv_label_set_text(ssid_label, "SSID");
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(ssid_label, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    s_ssid_ta = createLabeledTextarea(card, "Network name", false);
    lv_obj_align_to(s_ssid_ta, ssid_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    lv_obj_t *pass_label = lv_label_create(card);
    lv_label_set_text(pass_label, "Password");
    lv_obj_set_style_text_color(pass_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(pass_label, s_ssid_ta, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    s_pass_ta = createLabeledTextarea(card, "Password", true);
    lv_obj_align_to(s_pass_ta, pass_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    lv_obj_t *connect_btn = lv_btn_create(card);
    lv_obj_set_size(connect_btn, 140, 44);
    lv_obj_align_to(connect_btn, s_pass_ta, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 16);
    lv_obj_add_event_cb(connect_btn, connectBtnClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);

    s_forget_btn = lv_btn_create(card);
    lv_obj_set_size(s_forget_btn, 140, 44);
    lv_obj_set_style_bg_color(s_forget_btn, lv_color_hex(0xda3633), 0);
    lv_obj_align_to(s_forget_btn, connect_btn, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_event_cb(s_forget_btn, forgetBtnClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_forget_btn, LV_OBJ_FLAG_HIDDEN); // shown only for the connected network, see openModal()
    lv_obj_t *forget_label = lv_label_create(s_forget_btn);
    lv_label_set_text(forget_label, "Forget");
    lv_obj_center(forget_label);

    s_modal_status = lv_label_create(card);
    lv_label_set_text(s_modal_status, "");
    lv_obj_set_style_text_color(s_modal_status, lv_color_hex(0xf0a500), 0);
    lv_obj_align_to(s_modal_status, connect_btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lv_obj_set_width(s_modal_status, LV_PCT(100));
    lv_label_set_long_mode(s_modal_status, LV_LABEL_LONG_WRAP);
}

} // namespace

void init(lv_obj_t *screen)
{
    s_screen = screen;
    buildDropdown(screen);
    buildModal(screen);

    wifi_manager::onScanResults(onScanResults);
    wifi_manager::onStateChange(onWifiStateChange);
}

void toggleDropdown(lv_obj_t *anchor)
{
    bool hidden = lv_obj_has_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dropdown);
        wifi_manager::startScan();
    } else {
        lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace ui_wifi
