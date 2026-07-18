/*
 * APP STAGE ("the brains")
 * ------------------------
 * Boots into this stage from the `factory` splash (see firmware/factory),
 * or directly on subsequent boots once `otadata` points here. This is where
 * the LVGL UI, WiFi, RS485, and the on-device web server all run
 * concurrently as independent FreeRTOS tasks / event-driven callbacks:
 *
 *   - LVGL runs in its own task (started by lvgl_port_init()).
 *   - WiFi is entirely event-driven via esp_event (see wifi_manager.cpp);
 *     scans/connects never block.
 *   - RS485 has its own reader task (see rs485.cpp).
 *   - The web server (esp_http_server) is async under the hood and runs in
 *     its own task once started.
 *
 * app_main() itself only does one-time setup and then idles -- it never
 * blocks anything else.
 */
#include "esp_display_panel.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "lvgl_v8_port.h"
#include "machine_state.h"
#include "rs485.h"
#include "rs485_protocol.h"
#include "storage.h"
#include "ui/ui.h"
#include "web_server.h"
#include "wifi_manager.h"

using namespace esp_panel::board;

static const char *TAG = "app";

// board->begin() drives the CH422G IO-expander over I2C, which toggles the
// LCD backlight/reset lines and briefly spikes current draw. On a marginal
// 5V supply (e.g. some USB-A host ports/hubs/cables that can't sustain the
// draw), that spike can sag the rail enough to glitch the I2C write and
// fail `begin()` -- retrying with a short settle delay recovers from a
// one-off transient without masking a truly dead/miswired board (it still
// asserts if every attempt fails).
static bool begin_board_with_retries(Board *b, int max_attempts = 3)
{
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "board->begin() failed (attempt %d/%d), retrying after power-rail settle delay...",
                      attempt - 1, max_attempts);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (b->begin()) {
            return true;
        }
    }
    return false;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing board");
    Board *board = new Board();
    board->init();
    assert(begin_board_with_retries(board));

    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    ESP_LOGI(TAG, "Building UI");
    lvgl_port_lock(-1);
    ui::init(board);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Mounting storage");
    storage::init(board);

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager::init();
    wifi_manager::autoConnect();

    ESP_LOGI(TAG, "Starting RS485");
    rs485::init();
    rs485_protocol::init();
    machine_state::init();

    ESP_LOGI(TAG, "Starting web server");
    web_server::start();

    ESP_LOGI(TAG, "Setup complete, entering idle loop");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
