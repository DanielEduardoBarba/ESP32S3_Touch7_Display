#pragma once

#include "lvgl.h"

/**
 * The "Machine" scene: a grid of controls to play with -- a 0-100 dial, a
 * 0-1000 slider, a stepped +/- setpoint that can go negative, a mode
 * dropdown, four on/off toggles and a momentary pulse button -- all kept in
 * sync with an identical board over RS485 and with any WebSocket-connected
 * browsers (see machine_state.h for how that synchronization works). This
 * replaces "Home" as the default/first scene shown on the touchscreen.
 */
namespace ui_machine {

/** Builds the Machine scene as a child of `screen`, positioned below the
 *  header. Returns its root container so the scene manager can show/hide it. */
lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

/** Frees the whole widget tree (scene manager calls this when leaving the
 *  scene). Control values live in machine_state, so nothing is lost. */
void destroy();

} // namespace ui_machine
