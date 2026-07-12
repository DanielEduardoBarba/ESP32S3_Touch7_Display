#pragma once

#include "lvgl.h"

/** The single "Home" scene's content, shown below the header bar. */
namespace ui_home {

/** Builds the Home content area as a child of `screen`, positioned below
 *  the header (whose height is passed in so we don't overlap it). */
void build(lv_obj_t *screen, lv_coord_t header_height);

/** Brings the Home scene to the foreground. Currently a no-op since Home is
 *  the only scene, but kept as a real entry point for when more scenes are
 *  added behind the hamburger menu. */
void showHome();

} // namespace ui_home
