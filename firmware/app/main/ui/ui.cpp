#include "ui.h"

#include "lvgl.h"
#include "ui_header.h"
#include "ui_keyboard.h"
#include "ui_machine.h"
#include "ui_network.h"
#include "ui_scene_manager.h"
#include "ui_wifi.h"

namespace ui {

void init(esp_panel::board::Board *board)
{
    (void)board; // not currently needed directly by the UI layer

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101317), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t header_height = ui_header::build(screen);

    // Build both scenes up front (simpler and fast enough at this UI size
    // than lazily creating them on first visit); the scene manager then
    // just shows/hides whichever one is active.
    lv_obj_t *machine_root = ui_machine::build(screen, header_height);
    lv_obj_t *network_root = ui_network::build(screen, header_height);
    ui_scene_manager::registerScenes(machine_root, network_root);
    ui_scene_manager::show(ui_scene_manager::Scene::Machine); // Machine is the default/home scene

    ui_wifi::init(screen);
    ui_keyboard::init(screen);
}

} // namespace ui

