#include "ui_header.h"

#include "ui_scene_manager.h"
#include "ui_wifi.h"

namespace ui_header {
namespace {

constexpr lv_coord_t HEADER_HEIGHT = 56;

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

void buildHamburgerMenu(lv_obj_t *screen)
{
    s_hamburger_menu = lv_list_create(screen);
    lv_obj_set_size(s_hamburger_menu, 180, LV_SIZE_CONTENT);
    lv_obj_align(s_hamburger_menu, LV_ALIGN_TOP_LEFT, 8, 64);
    lv_obj_add_flag(s_hamburger_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_hamburger_menu, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(s_hamburger_menu, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(s_hamburger_menu, 1, 0);

    // Two scenes today: Machine (the default view) and Network (WiFi/device
    // status). Add more lv_list_add_btn() entries here if more scenes are
    // added later.
    lv_obj_t *machine_item = lv_list_add_btn(s_hamburger_menu, LV_SYMBOL_SETTINGS, "Machine");
    lv_obj_add_event_cb(machine_item, machineMenuItemClickedCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *network_item = lv_list_add_btn(s_hamburger_menu, LV_SYMBOL_WIFI, "Network");
    lv_obj_add_event_cb(network_item, networkMenuItemClickedCb, LV_EVENT_CLICKED, nullptr);
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
