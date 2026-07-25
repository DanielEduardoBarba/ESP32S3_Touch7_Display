#pragma once

#include "lvgl.h"

/**
 * Header Bluetooth icon (left of Wi-Fi): a dropdown holding the whole BLE
 * flow -- radio on/off, "discoverable" (so a phone can find this board),
 * a scan list of nearby devices, and connect / pair / disconnect / forget
 * for each one. Pairing confirmations appear in a modal on top.
 *
 * See ble_manager.h for the protocol side; this file is only widgets.
 */
namespace ui_bluetooth {

void init(lv_obj_t *screen);

/** Shows/hides the dropdown (and kicks off a scan when opened). */
void toggleDropdown(lv_obj_t *anchor);

/** Hides it (used when another header dropdown opens). */
void hideDropdown();

} // namespace ui_bluetooth
