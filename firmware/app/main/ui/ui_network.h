#pragma once

#include "lvgl.h"

/** The "Network" scene: WiFi/device status. This is the previous "Home"
 *  scene's content, renamed and moved to its own menu entry now that
 *  "Machine" is the default scene. */
namespace ui_network {

/** Builds the Network scene as a child of `screen`, positioned below the
 *  header. Returns its root container so the scene manager can show/hide it. */
lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

} // namespace ui_network
