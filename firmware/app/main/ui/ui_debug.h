#pragma once

#include "lvgl.h"

/** "Debug" scene: live view of the same log lines printed to the console
 *  (via log_store's vprintf split). */
namespace ui_debug {

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

} // namespace ui_debug
