#pragma once

#include <cstdint>

namespace esp_panel::drivers {
class Backlight;
}

/**
 * Local display settings -- currently just the backlight brightness behind
 * the header's "i" (info/display) dropdown.
 *
 * Deliberately NOT shared with the peer board over the RS485 link: screen
 * brightness is a property of where a panel physically sits (glare, viewing
 * angle, night shift), not of the machine state the two boards agree on.
 * Contrast this with machine_state.h (mirrored to the peer) and wifi_sync.h
 * (credentials mirrored to the peer).
 *
 * The value is persisted in NVS via user_store, so a board comes back at the
 * brightness its operator left it at.
 */
namespace display_settings {

/** Never allow a fully dark screen: at 0% the panel looks broken and the UI
 *  to turn it back up is invisible. */
constexpr uint8_t BRIGHTNESS_MIN = 10;
constexpr uint8_t BRIGHTNESS_MAX = 100;
constexpr uint8_t BRIGHTNESS_DEFAULT = 100;

/** Takes the panel's backlight driver (from Board::getBacklight(), may be
 *  null on boards without one), restores the persisted brightness and
 *  applies it. Call once, after the UI has drawn its first frame -- this
 *  replaces the plain backlight->on() that used to end the boot sequence. */
void init(esp_panel::drivers::Backlight *backlight);

uint8_t brightness();

/** Applies `percent` (clamped to the range above) to the panel. `persist`
 *  is false while a slider is being dragged, so NVS sees one write when the
 *  user lets go instead of one per pixel of movement. */
void setBrightness(uint8_t percent, bool persist = true);

} // namespace display_settings
