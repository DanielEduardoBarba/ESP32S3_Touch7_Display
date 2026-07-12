#pragma once

namespace esp_panel::board {
class Board;
}

/** Builds the whole local touchscreen UI: header bar + Home scene + the
 *  WiFi dropdown/modal + the on-screen keyboard. Call once after
 *  lvgl_port_init(). */
namespace ui {

void init(esp_panel::board::Board *board);

} // namespace ui
