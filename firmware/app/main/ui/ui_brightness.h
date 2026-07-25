#pragma once

#include "lvgl.h"

/**
 * Header "i" icon: a small dropdown (same look as the Wi-Fi one) holding
 * this board's LOCAL display settings -- currently the backlight slider.
 *
 * Nothing here travels over the peer link: see display_settings.h for why
 * brightness is deliberately per-board.
 */
namespace ui_brightness {

/** Creates the (hidden) dropdown as a child of `screen`. Call once during
 *  UI setup. */
void init(lv_obj_t *screen);

/** Shows/hides the dropdown, anchored below the header's info icon. */
void toggleDropdown(lv_obj_t *anchor);

/** Hides it (used when another header dropdown opens). */
void hideDropdown();

} // namespace ui_brightness
