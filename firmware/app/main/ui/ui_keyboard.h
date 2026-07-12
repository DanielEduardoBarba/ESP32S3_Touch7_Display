#pragma once

#include "lvgl.h"

/**
 * Shared full-screen "iPhone-like" QWERTY keyboard (built on LVGL's stock
 * lv_keyboard widget, which already provides shift/caps-lock and a
 * special-characters ("123#") mode out of the box). Any text area in the UI
 * can grab it via attach(); it detaches and hides itself automatically when
 * the user taps "OK"/"Enter", "Cancel", or focuses away.
 */
namespace ui_keyboard {

/** Creates the (initially hidden) keyboard as a child of `parent` (normally
 *  the top-level screen, so it floats above everything else). Call once. */
void init(lv_obj_t *parent);

/** Shows the keyboard and binds it to the given text area. */
void attach(lv_obj_t *textarea);

/** Hides the keyboard and unbinds it from whatever text area was using it. */
void hide();

} // namespace ui_keyboard
