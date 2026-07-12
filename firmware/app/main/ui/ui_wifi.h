#pragma once

#include "lvgl.h"

/**
 * Top-right "network" icon: opens a dropdown of scanned WiFi access points;
 * tapping one opens a modal with editable SSID/password fields and a
 * Connect button. Entirely event-driven -- scans and connection attempts
 * are kicked off on wifi_manager and the UI updates later via its
 * callbacks, so the LVGL task never blocks waiting on WiFi.
 */
namespace ui_wifi {

/** Creates the (hidden) dropdown + connect modal as children of `screen`,
 *  and wires up wifi_manager callbacks. Call once during UI setup. */
void init(lv_obj_t *screen);

/** Shows/hides the AP dropdown, anchored below `anchor` (the header's
 *  network icon button). Triggers a fresh scan each time it's opened. */
void toggleDropdown(lv_obj_t *anchor);

} // namespace ui_wifi
