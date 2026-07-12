#include "ui.h"

#include "lvgl.h"
#include "ui_header.h"
#include "ui_home.h"
#include "ui_keyboard.h"
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
    ui_home::build(screen, header_height);
    ui_wifi::init(screen);
    ui_keyboard::init(screen);
}

} // namespace ui
