/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 *
 * Vendored/adapted from esp-arduino-libs/ESP32_Display_Panel
 * examples/esp_idf/lvgl_v8_port (duplicated into both the `factory` and
 * `app` firmware projects since ESP-IDF component sharing across separate
 * top-level projects adds more ceremony than it's worth for ~250 lines).
 */
#pragma once

#include "esp_display_panel.hpp"
#include "lvgl.h"

// *INDENT-OFF*

#define LVGL_PORT_TICK_PERIOD_MS                (2)

// Draw buffers live in PSRAM: two 20-row buffers at 1024px wide are ~80KB,
// which starved internal RAM (task stacks, WiFi, httpd can ONLY use
// internal). Octal PSRAM @80MHz renders fast enough -- check the on-screen
// FPS monitor if in doubt.
#define LVGL_PORT_BUFFER_MALLOC_CAPS            (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LVGL_PORT_BUFFER_SIZE_HEIGHT            (20)
#define LVGL_PORT_BUFFER_NUM                    (2)

#define LVGL_PORT_TASK_MAX_DELAY_MS             (500)
#define LVGL_PORT_TASK_MIN_DELAY_MS             (2)
#define LVGL_PORT_TASK_STACK_SIZE               (6 * 1024)
#define LVGL_PORT_TASK_PRIORITY                 (2)
#define LVGL_PORT_TASK_CORE                     (0)

// *INDENT-ON*

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Porting LVGL with LCD and touch panel. Call after LCD/touch init.
 */
bool lvgl_port_init(esp_panel::drivers::LCD *lcd, esp_panel::drivers::Touch *tp);

/**
 * @brief Deinitialize the LVGL porting.
 */
bool lvgl_port_deinit(void);

/**
 * @brief Lock the LVGL mutex before calling LVGL APIs outside of the LVGL task.
 */
bool lvgl_port_lock(int timeout_ms);

/**
 * @brief Unlock the LVGL mutex.
 */
bool lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
