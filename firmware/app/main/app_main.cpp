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
#include "rs485.h"
#include "storage.h"
#include "ui/ui.h"
#include "web_server.h"
#include "wifi_manager.h"

using namespace esp_panel::board;

static const char *TAG = "app";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing board");
    Board *board = new Board();
    board->init();
    assert(board->begin());

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

    ESP_LOGI(TAG, "Starting web server");
    web_server::start();

    ESP_LOGI(TAG, "Setup complete, entering idle loop");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
