/*
 * FACTORY STAGE (a.k.a. the "bootloader splash" stage)
 * -----------------------------------------------------
 * This is the very first application that runs after the ESP32's own
 * (real) 2nd-stage bootloader. It intentionally does as little as possible:
 *
 *   1. Bring up the display/touch panel and LVGL.
 *   2. Show a splash screen with a placeholder "Continue" UI. This is where
 *      a future field-update flow (e.g. "hold to enter recovery / firmware
 *      upload mode") can be built out, WITHOUT needing to touch the real
 *      application or re-flash via the ESP-IDF toolchain.
 *   3. When the user presses "Continue", point the OTA boot selector at the
 *      `ota_0` partition (the real application, built from firmware/app)
 *      and reboot into it.
 *
 * Because this stage and the app stage are separate OTA partitions (see
 * firmware/partitions.csv), the app's ota_0/ota_1 slots can later be
 * re-flashed independently (e.g. from the app's own web UI, see
 * components/web_server/ota_handler in firmware/app) without ever touching
 * this splash stage.
 */
#include <cstdio>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_display_panel.hpp"
#include "lvgl.h"
#include "lvgl_v8_port.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static const char *TAG = "factory";

static Board *board = nullptr;
static lv_obj_t *status_label = nullptr;

static void boot_into_app()
{
    // Boot into the first OTA slot (`ota_0`) by default. On later boots the
    // app itself (or a future OTA update) is free to switch to `ota_1`; this
    // factory stage is only reached again after a factory-reset that clears
    // the `otadata` partition.
    const esp_partition_t *part = esp_partition_find_first(
                                       ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0"
                                   );
    if (part == nullptr) {
        ESP_LOGE(TAG, "Could not find ota_0 partition!");
        if (status_label) {
            lv_label_set_text(status_label, "Error: app partition not found");
        }
        return;
    }

    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        if (status_label) {
            lv_label_set_text(status_label, "Error: could not select app partition");
        }
        return;
    }

    ESP_LOGI(TAG, "Booting into ota_0...");
    vTaskDelay(pdMS_TO_TICKS(150)); // let the label render / logs flush
    esp_restart();
}

static void continue_btn_event_cb(lv_event_t *e)
{
    if (status_label) {
        lv_label_set_text(status_label, "Starting application...");
    }
    lv_refr_now(nullptr);
    boot_into_app();
}

static void build_splash_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101317), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Waveshare ESP32-S3 Touch LCD 7");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Factory bootstrap");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    /*
     * Placeholder area: this is where a future "hold here to enter recovery /
     * upload new firmware over USB or SD" flow can be implemented, so field
     * units can be updated without the ESP-IDF toolchain.
     */
    lv_obj_t *placeholder = lv_obj_create(scr);
    lv_obj_set_size(placeholder, 520, 180);
    lv_obj_align(placeholder, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(placeholder, 12, 0);
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(placeholder, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(placeholder, 1, 0);
    lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *placeholder_label = lv_label_create(placeholder);
    lv_label_set_text(placeholder_label,
                       "Recovery / firmware update UI goes here\n(placeholder)");
    lv_obj_set_style_text_align(placeholder_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(placeholder_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_center(placeholder_label);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 220, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_event_cb(btn, continue_btn_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Continue");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_label);

    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xf0a500), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing board");

    board = new Board();
    board->init();
    assert(board->begin());

    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    lvgl_port_lock(-1);
    build_splash_ui();
    lvgl_port_unlock();

    // All real work happens in the LVGL task started by lvgl_port_init().
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
