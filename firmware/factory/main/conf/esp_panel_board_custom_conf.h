/*
 * This file only exists so ESP32_Display_Panel's `__has_include("esp_panel_board_custom_conf.h")`
 * check finds it (see the repo root README.md's "Board variants" section, and
 * docs/envs/use_with_idf.md in the espressif__esp32_display_panel component
 * for why an ESP-IDF project needs this indirection instead of just
 * dropping the header into main/). The actual board definition is shared
 * with the app-stage project so both stay in sync.
 *
 * Only takes effect when `./build.sh --variant 7b ...` is used (see
 * firmware/common/sdkconfig.defaults.7b) -- for the default "7" variant,
 * this file is still visible to the compiler but its content is inert.
 */
#pragma once

#include "../../../common/esp_panel_board_custom_conf.7b.h"
