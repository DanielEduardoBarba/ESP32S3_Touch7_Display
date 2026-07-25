#include "ui_bluetooth.h"

#include <string>
#include <vector>

#include "app_config.h"
#include "ble_manager.h"
#include "lvgl_v8_port.h"

namespace ui_bluetooth {
namespace {

lv_obj_t *s_dropdown = nullptr;
lv_obj_t *s_enable_switch = nullptr;
lv_obj_t *s_discover_switch = nullptr;
lv_obj_t *s_name_label = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_scan_btn = nullptr;
lv_obj_t *s_list = nullptr;

// Pairing confirmation modal.
lv_obj_t *s_modal_bg = nullptr;
lv_obj_t *s_modal_passkey = nullptr;
lv_obj_t *s_modal_text = nullptr;
lv_obj_t *s_modal_accept_btn = nullptr;
lv_obj_t *s_modal_reject_btn = nullptr;

/** Address of the device each list row refers to. Owned by the row (freed
 *  with it via LV_EVENT_DELETE), so rebuilding the list can't leak. */
void rowDeletedCb(lv_event_t *e)
{
    delete static_cast<std::string *>(lv_event_get_user_data(e));
}

const char *stateText(ble_manager::State state)
{
    switch (state) {
    case ble_manager::State::Off:        return "Off";
    case ble_manager::State::Idle:       return "Ready";
    case ble_manager::State::Scanning:   return "Scanning...";
    case ble_manager::State::Connecting: return "Connecting...";
    case ble_manager::State::Connected:  return "Connected";
    case ble_manager::State::Failed:     return "Failed";
    }
    return "";
}

void setStatus(const std::string &text, lv_color_t color)
{
    lv_label_set_text(s_status_label, text.c_str());
    lv_obj_set_style_text_color(s_status_label, color, 0);
}

void clearList()
{
    lv_obj_clean(s_list);
}

// --- Actions ---------------------------------------------------------------

void enableSwitchCb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_enable_switch, LV_STATE_CHECKED);
    ble_manager::setEnabled(on);
    if (on) {
        // The stack needs a moment to sync before a scan can start; the
        // state callback re-enables the button when it is ready.
        setStatus("Starting Bluetooth...", lv_color_hex(0xf0a500));
    } else {
        clearList();
        setStatus("Off", lv_color_hex(0x9aa4b2));
    }
    lv_obj_add_state(s_scan_btn, on ? LV_STATE_DEFAULT : LV_STATE_DISABLED);
}

void discoverSwitchCb(lv_event_t *e)
{
    ble_manager::setDiscoverable(lv_obj_has_state(s_discover_switch, LV_STATE_CHECKED));
}

void scanBtnCb(lv_event_t *e)
{
    if (!ble_manager::enabled()) {
        return;
    }
    ble_manager::startScan(APP_BLE_SCAN_SECONDS);
}

void deviceRowCb(lv_event_t *e)
{
    auto *addr = static_cast<std::string *>(lv_event_get_user_data(e));
    if (addr == nullptr) {
        return;
    }
    ble_manager::DeviceInfo connected = ble_manager::connectedDevice();
    if (ble_manager::state() == ble_manager::State::Connected && connected.addr == *addr) {
        ble_manager::disconnect();
    } else {
        ble_manager::connect(*addr);
    }
}

void forgetBtnCb(lv_event_t *e)
{
    auto *addr = static_cast<std::string *>(lv_event_get_user_data(e));
    if (addr != nullptr) {
        ble_manager::forget(*addr);
    }
}

void pairAcceptCb(lv_event_t *e)
{
    ble_manager::confirmPairing(true);
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN);
}

void pairRejectCb(lv_event_t *e)
{
    ble_manager::confirmPairing(false);
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN);
}

// --- ble_manager callbacks (run on the NimBLE host task) --------------------

void onScanResults(const std::vector<ble_manager::DeviceInfo> &devices)
{
    lvgl_port_lock(-1);
    clearList();

    ble_manager::DeviceInfo connected = ble_manager::connectedDevice();
    bool is_connected = ble_manager::state() == ble_manager::State::Connected;

    if (devices.empty()) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, ble_manager::enabled() ? "No devices yet -- tap Scan"
                                                        : "Bluetooth is off");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x6e7681), 0);
        lvgl_port_unlock();
        return;
    }

    for (const auto &device : devices) {
        std::string label = device.name.empty() ? device.addr : device.name;
        label += "   " + std::to_string((int)device.rssi) + "dBm";
        if (device.bonded) {
            label += "  " LV_SYMBOL_OK;
        }
        if (is_connected && device.addr == connected.addr) {
            label += "  (connected)";
        }

        lv_obj_t *row = lv_list_add_btn(s_list, LV_SYMBOL_BLUETOOTH, label.c_str());
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2128), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x272e37), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, lv_color_hex(0xe6edf3), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        if (is_connected && device.addr == connected.addr) {
            lv_obj_set_style_border_color(row, lv_color_hex(0x2ea043), 0);
            lv_obj_set_style_border_width(row, 2, 0);
        }

        auto *addr_copy = new std::string(device.addr);
        lv_obj_add_event_cb(row, deviceRowCb, LV_EVENT_CLICKED, addr_copy);
        lv_obj_add_event_cb(row, rowDeletedCb, LV_EVENT_DELETE, addr_copy);

        if (device.bonded) {
            // A bonded device gets a "forget" affordance, otherwise the only
            // way to re-pair from scratch would be to wipe NVS.
            auto *forget_addr = new std::string(device.addr);
            lv_obj_t *forget_btn = lv_btn_create(row);
            lv_obj_set_size(forget_btn, 28, 28);
            lv_obj_align(forget_btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(forget_btn, lv_color_hex(0x3d2b2b), 0);
            lv_obj_add_event_cb(forget_btn, forgetBtnCb, LV_EVENT_CLICKED, forget_addr);
            lv_obj_add_event_cb(forget_btn, rowDeletedCb, LV_EVENT_DELETE, forget_addr);
            lv_obj_t *x = lv_label_create(forget_btn);
            lv_label_set_text(x, LV_SYMBOL_TRASH);
            lv_obj_center(x);
        }
    }
    lvgl_port_unlock();
}

void onStateChange(ble_manager::State state, const ble_manager::DeviceInfo &device,
                   const std::string &detail)
{
    lvgl_port_lock(-1);
    std::string text = stateText(state);
    if (state == ble_manager::State::Connected) {
        text += ": " + (device.name.empty() ? device.addr : device.name);
        if (!detail.empty()) {
            text += " (" + detail + ")";
        }
    } else if (!detail.empty() && state == ble_manager::State::Failed) {
        text += ": " + detail;
    }

    lv_color_t color = lv_color_hex(0x9aa4b2);
    if (state == ble_manager::State::Connected) {
        color = lv_color_hex(0x3fb950);
    } else if (state == ble_manager::State::Failed) {
        color = lv_color_hex(0xf85149);
    } else if (state == ble_manager::State::Scanning || state == ble_manager::State::Connecting) {
        color = lv_color_hex(0xf0a500);
    }
    setStatus(text, color);

    if (state != ble_manager::State::Off) {
        lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
    }
    // A connection/disconnection changes how rows should be drawn.
    lvgl_port_unlock();
}

void onPasskeyRequest(uint32_t passkey, bool numeric_comparison)
{
    lvgl_port_lock(-1);
    lv_label_set_text_fmt(s_modal_passkey, "%06u", (unsigned)passkey);
    if (numeric_comparison) {
        lv_label_set_text(s_modal_text,
                          "Does the other device show this same number?");
        lv_obj_clear_flag(s_modal_accept_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_modal_reject_btn, 0), "No");
    } else {
        lv_label_set_text(s_modal_text, "Enter this passkey on the other device.");
        lv_obj_add_flag(s_modal_accept_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_modal_reject_btn, 0), "Close");
    }
    lv_obj_clear_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal_bg);
    lvgl_port_unlock();
}

// --- Building ---------------------------------------------------------------

lv_obj_t *addSwitchRow(lv_obj_t *parent, const char *text, bool checked, lv_event_cb_t cb,
                       lv_obj_t **switch_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xe6edf3), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 48, 24);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    *switch_out = sw;
    return row;
}

void buildDropdown(lv_obj_t *screen)
{
    s_dropdown = lv_obj_create(screen);
    lv_obj_set_size(s_dropdown, 320, 380);
    lv_obj_align(s_dropdown, LV_ALIGN_TOP_RIGHT, -8, 64);
    lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_dropdown, 12, 0);
    lv_obj_set_style_clip_corner(s_dropdown, true, 0);
    lv_obj_set_style_bg_color(s_dropdown, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(s_dropdown, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_dropdown, 1, 0);
    lv_obj_set_style_pad_all(s_dropdown, 12, 0);
    lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_dropdown, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dropdown, 6, 0);

    lv_obj_t *title = lv_label_create(s_dropdown);
    lv_label_set_text(title, "Bluetooth");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    addSwitchRow(s_dropdown, "Bluetooth", ble_manager::enabled(), enableSwitchCb, &s_enable_switch);
    addSwitchRow(s_dropdown, "Discoverable", ble_manager::discoverable(), discoverSwitchCb,
                 &s_discover_switch);

    s_name_label = lv_label_create(s_dropdown);
    lv_label_set_text_fmt(s_name_label, "Visible as: %s", ble_manager::deviceName().c_str());
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0x6e7681), 0);

    s_status_label = lv_label_create(s_dropdown);
    lv_obj_set_width(s_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    setStatus(stateText(ble_manager::state()), lv_color_hex(0x9aa4b2));

    s_scan_btn = lv_btn_create(s_dropdown);
    lv_obj_set_size(s_scan_btn, LV_PCT(100), 36);
    lv_obj_add_event_cb(s_scan_btn, scanBtnCb, LV_EVENT_CLICKED, nullptr);
    if (!ble_manager::enabled()) {
        lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    }
    lv_obj_t *scan_label = lv_label_create(s_scan_btn);
    lv_label_set_text(scan_label, LV_SYMBOL_REFRESH "  Scan for devices");
    lv_obj_center(scan_label);

    s_list = lv_list_create(s_dropdown);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
}

void buildPairingModal(lv_obj_t *screen)
{
    s_modal_bg = lv_obj_create(screen);
    lv_obj_remove_style_all(s_modal_bg);
    lv_obj_set_size(s_modal_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_bg, LV_OPA_50, 0);
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_modal_bg);
    lv_obj_set_size(card, 420, 280);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Bluetooth pairing");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_modal_passkey = lv_label_create(card);
    lv_obj_set_style_text_font(s_modal_passkey, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_modal_passkey, lv_color_hex(0x58a6ff), 0);
    lv_obj_align(s_modal_passkey, LV_ALIGN_TOP_MID, 0, 50);

    s_modal_text = lv_label_create(card);
    lv_obj_set_width(s_modal_text, LV_PCT(100));
    lv_label_set_long_mode(s_modal_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_modal_text, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align(s_modal_text, LV_ALIGN_TOP_MID, 0, 110);

    s_modal_accept_btn = lv_btn_create(card);
    lv_obj_set_size(s_modal_accept_btn, 150, 44);
    lv_obj_align(s_modal_accept_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_modal_accept_btn, lv_color_hex(0x238636), 0);
    lv_obj_add_event_cb(s_modal_accept_btn, pairAcceptCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *accept_label = lv_label_create(s_modal_accept_btn);
    lv_label_set_text(accept_label, "Pair");
    lv_obj_center(accept_label);

    s_modal_reject_btn = lv_btn_create(card);
    lv_obj_set_size(s_modal_reject_btn, 150, 44);
    lv_obj_align(s_modal_reject_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(s_modal_reject_btn, pairRejectCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reject_label = lv_label_create(s_modal_reject_btn);
    lv_label_set_text(reject_label, "No");
    lv_obj_center(reject_label);
}

} // namespace

void init(lv_obj_t *screen)
{
    buildDropdown(screen);
    buildPairingModal(screen);

    ble_manager::onScanResults(onScanResults);
    ble_manager::onStateChange(onStateChange);
    ble_manager::onPasskeyRequest(onPasskeyRequest);

    onScanResults({}); // draw the empty-state hint
}

void toggleDropdown(lv_obj_t *anchor)
{
    (void)anchor;
    if (lv_obj_has_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dropdown);
        if (ble_manager::enabled()) {
            ble_manager::startScan(APP_BLE_SCAN_SECONDS);
        }
    } else {
        lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
        ble_manager::stopScan();
    }
}

void hideDropdown()
{
    if (s_dropdown != nullptr) {
        lv_obj_add_flag(s_dropdown, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace ui_bluetooth
