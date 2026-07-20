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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "boot_health.h"
#include "comm_protocol.h"
#include "dev_console.h"
#include "fw_update.h"
#include "log_store.h"
#include "lvgl_v8_port.h"
#include "machine_state.h"
#include "ports.h"
#include "storage.h"
#include "ui/ui.h"
#include "user_store.h"
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
    // Recovery escape hatch: if the BOOT button is held while the app
    // starts, reboot into the factory/recovery app instead. Must run before
    // board init because GPIO0 becomes an LCD data line afterwards.
    boot_health::checkRecoveryButtonAtBoot();

    // Split the log stream into the console + the Debug scene's ring buffer
    // as early as possible so the buffer catches the whole boot.
    log_store::init();

    ESP_LOGI(TAG, "Initializing board");
    Board *board = new Board();
    board->init();
    assert(begin_board_with_retries(board));

    // Kill the "white flash": the panel shows garbage/white between the
    // backlight coming up (inside begin()) and the first real LVGL frame.
    // Turn the backlight off immediately and back on only after the splash
    // has actually been rendered below.
    auto *backlight = board->getBacklight();
    if (backlight != nullptr) {
        backlight->off();
    }

    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    ESP_LOGI(TAG, "Initializing user data store");
    user_store::init();

    ESP_LOGI(TAG, "Building UI");
    lvgl_port_lock(-1);
    ui::init(board);
    lv_refr_now(nullptr); // splash is on screen before the backlight returns
    lvgl_port_unlock();
    if (backlight != nullptr) {
        backlight->on();
    }

    ESP_LOGI(TAG, "Mounting storage");
    storage::init(board);

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager::init();
    wifi_manager::autoConnect();

    ESP_LOGI(TAG, "Starting peer link (ports + framing + machine sync + fw update)");
    ports::init();          // transports (RS485 default; see ports_config.h)
    comm_protocol::init();  // STX/ETX framing + hex logging on top of ports
    machine_state::init();  // dial/toggle sync over the framing
    fw_update::init();      // device-to-device OTA over the framing
    dev_console::init();    // dev builds: serial command hooks for tooling

    ESP_LOGI(TAG, "Starting web server");
    web_server::start();

    // Every critical subsystem is up -- that's the OTA self-test. If this
    // image was just installed over-the-air (pending verify), lock it in;
    // had we crashed anywhere above, the stock bootloader would have rolled
    // back to the previous working image on the next reset instead.
    boot_health::commitRunningImageIfPending();

    ESP_LOGI(TAG, "Setup complete, entering idle loop (internal heap free: %u, min ever: %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
