#pragma once

#include "lvgl.h"

/**
 * The "Machine" scene: a 0-100 dial and a sample toggle switch, both kept in
 * sync with an identical board over RS485 and with any WebSocket-connected
 * browsers (see machine_state.h for how that synchronization works). This
 * replaces "Home" as the default/first scene shown on the touchscreen.
 */
namespace ui_machine {

/** Builds the Machine scene as a child of `screen`, positioned below the
 *  header. Returns its root container so the scene manager can show/hide it. */
lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

} // namespace ui_machine
