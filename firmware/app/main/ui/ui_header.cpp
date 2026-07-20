#include "ui_header.h"

#include "app_config.h"
#include "ui_scene_manager.h"
#include "ui_wifi.h"

namespace ui_header {
namespace {

constexpr lv_coord_t HEADER_HEIGHT = APP_UI_HEADER_HEIGHT;

lv_obj_t *s_hamburger_menu = nullptr;

void hideHamburgerMenu()
{
    lv_obj_add_flag(s_hamburger_menu, LV_OBJ_FLAG_HIDDEN);
}

void hamburgerBtnClickedCb(lv_event_t *e)
{
    bool hidden = lv_obj_has_flag(s_hamburger_menu, LV_OBJ_FLAG_HIDDEN);
    if (hidden) {
        lv_obj_clear_flag(s_hamburger_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_hamburger_menu);
    } else {
        hideHamburgerMenu();
    }
}

void machineMenuItemClickedCb(lv_event_t *e)
{
    ui_scene_manager::show(ui_scene_manager::Scene::Machine);
    hideHamburgerMenu();
}

void networkMenuItemClickedCb(lv_event_t *e)
{
    ui_scene_manager::show(ui_scene_manager::Scene::Network);
    hideHamburgerMenu();
}

void portsMenuItemClickedCb(lv_event_t *e)
{
    ui_scene_manager::show(ui_scene_manager::Scene::Ports);
    hideHamburgerMenu();
}

void updateMenuItemClickedCb(lv_event_t *e)
{
    ui_scene_manager::show(ui_scene_manager::Scene::Update);
    hideHamburgerMenu();
}

void debugMenuItemClickedCb(lv_event_t *e)
{
    ui_scene_manager::show(ui_scene_manager::Scene::Debug);
    hideHamburgerMenu();
}

void homeBtnClickedCb(lv_event_t *e)
{
    // The header's Home icon always jumps to the Machine scene, since that's
    // now the app's default/primary view (Network is reached via the menu
    // or the WiFi icon's own dropdown).
    ui_scene_manager::show(ui_scene_manager::Scene::Machine);
}

void infoBtnClickedCb(lv_event_t *e)
{
    // Placeholder: intentionally does nothing yet.
}

void networkBtnClickedCb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    ui_wifi::toggleDropdown(btn);
}

lv_obj_t *createIconButton(lv_obj_t *parent, const char *symbol_or_text)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x272e37), 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol_or_text);
    lv_obj_center(label);
    return btn;
}

/** lv_list buttons default to a white background with their own rounding,
 *  which clashes with the dark card. Make every menu item blend into the
 *  card (same bg, square corners -- the LIST clips its rounded corners)
 *  with a subtle pressed shade. */
lv_obj_t *addMenuItem(lv_obj_t *list, const char *symbol, const char *text,
                      lv_event_cb_t cb)
{
    lv_obj_t *item = lv_list_add_btn(list, symbol, text);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x272e37), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(item, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_radius(item, 0, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, nullptr);
    return item;
}

void buildHamburgerMenu(lv_obj_t *screen)
{
    s_hamburger_menu = lv_list_create(screen);
    lv_obj_set_size(s_hamburger_menu, 180, LV_SIZE_CONTENT);
    lv_obj_align(s_hamburger_menu, LV_ALIGN_TOP_LEFT, 8, HEADER_HEIGHT + 8);
    lv_obj_add_flag(s_hamburger_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(s_hamburger_menu, 12, 0);
    lv_obj_set_style_clip_corner(s_hamburger_menu, true, 0);
    lv_obj_set_style_bg_color(s_hamburger_menu, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(s_hamburger_menu, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_hamburger_menu, 1, 0);
    lv_obj_set_style_pad_all(s_hamburger_menu, 0, 0);

    // Scenes: Machine (default view), Network (WiFi/device status), Ports
    // (peer-link transport selection), Update (device-to-device firmware
    // update), Debug (live log view). Add more addMenuItem() calls here
    // for future scenes.
    addMenuItem(s_hamburger_menu, LV_SYMBOL_SETTINGS, "Machine", machineMenuItemClickedCb);
    addMenuItem(s_hamburger_menu, LV_SYMBOL_WIFI, "Network", networkMenuItemClickedCb);
    addMenuItem(s_hamburger_menu, LV_SYMBOL_USB, "Ports", portsMenuItemClickedCb);
    addMenuItem(s_hamburger_menu, LV_SYMBOL_DOWNLOAD, "Update", updateMenuItemClickedCb);
    addMenuItem(s_hamburger_menu, LV_SYMBOL_LIST, "Debug", debugMenuItemClickedCb);
}

} // namespace

lv_coord_t build(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(header, 8, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hamburger_btn = createIconButton(header, LV_SYMBOL_BARS);
    lv_obj_align(hamburger_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(hamburger_btn, hamburgerBtnClickedCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *home_btn = createIconButton(header, LV_SYMBOL_HOME);
    lv_obj_align_to(home_btn, hamburger_btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_event_cb(home_btn, homeBtnClickedCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *version_label = lv_label_create(header);
    lv_label_set_text(version_label, "v" APP_VERSION);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0x8b949e), 0);
    lv_obj_align_to(version_label, home_btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t *info_btn = createIconButton(header, "i");
    lv_obj_align(info_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(info_btn, infoBtnClickedCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *network_btn = createIconButton(header, LV_SYMBOL_WIFI);
    lv_obj_align_to(network_btn, info_btn, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_add_event_cb(network_btn, networkBtnClickedCb, LV_EVENT_CLICKED, nullptr);

    buildHamburgerMenu(screen);

    return HEADER_HEIGHT;
}

} // namespace ui_header
