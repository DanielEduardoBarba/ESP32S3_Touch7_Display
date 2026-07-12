#pragma once

#include "lvgl.h"

/**
 * Top header bar: hamburger menu (dropdown with a single "Home" entry) +
 * Home icon on the left; network icon (WiFi dropdown, see ui_wifi.h) + info
 * icon (placeholder, currently a no-op) on the right.
 */
namespace ui_header {

/** Builds the header bar as a child of `screen`. Returns its height in
 *  pixels so the caller can lay out the content area below it. */
lv_coord_t build(lv_obj_t *screen);

} // namespace ui_header
