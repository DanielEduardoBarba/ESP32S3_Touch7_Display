#include "ui.h"

#include "lvgl.h"
#include "../../../common/dev_monitor.h"
#include "ui_bluetooth.h"
#include "ui_brightness.h"
#include "ui_debug.h"
#include "ui_header.h"
#include "ui_keyboard.h"
#include "ui_machine.h"
#include "ui_network.h"
#include "ui_ports.h"
#include "ui_scene_manager.h"
#include "ui_update.h"
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

    // Lazy scene memory management (see ui_scene_manager.h): only the scene
    // on screen keeps widgets alive; leaving a scene frees its whole tree.
    // The Update scene is the exception -- persistent, so device-to-device
    // update/sync UI keeps working from any scene.
    using Scene = ui_scene_manager::Scene;
    ui_scene_manager::init(screen, header_height);
    ui_scene_manager::registerScene(Scene::Machine, ui_machine::build, ui_machine::destroy);
    ui_scene_manager::registerScene(Scene::Network, ui_network::build, ui_network::destroy);
    ui_scene_manager::registerScene(Scene::Ports, ui_ports::build, ui_ports::destroy);
    ui_scene_manager::registerScene(Scene::Update, ui_update::build, nullptr, /*persistent=*/true);
    ui_scene_manager::registerScene(Scene::Debug, ui_debug::build, ui_debug::destroy);
    ui_scene_manager::show(Scene::Machine); // Machine is the default/home scene

    ui_wifi::init(screen);
    ui_bluetooth::init(screen);   // header Bluetooth icon (left of Wi-Fi)
    ui_brightness::init(screen);  // header "i" icon: local display settings
    ui_keyboard::init(screen);

    dev_monitor::show(); // FPS/CPU/RAM/PSRAM card (dev builds only)
}

} // namespace ui

