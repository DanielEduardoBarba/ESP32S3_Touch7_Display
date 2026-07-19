/*
 * FACTORY / RECOVERY APP
 * ----------------------
 * The small "rescue firmware" of the high-reliability boot architecture.
 * Boot flow (all standard ESP-IDF -- ROM bootloader -> stock 2nd-stage
 * bootloader -> otadata slot selection -> this app or an OTA slot):
 *
 *   - Fresh flash / factory reset: otadata is empty, so the bootloader runs
 *     this app. It shows a short countdown and then boots the main app
 *     automatically -- no user interaction needed for normal bring-up.
 *
 *   - Forced recovery: holding the BOOT button while the MAIN app starts
 *     makes it reboot into this app (see firmware/app/main/boot_health.cpp).
 *     Tapping the screen during the countdown stops it and keeps the
 *     recovery menu open.
 *
 *   - Automatic recovery: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an
 *     OTA image that crashes before validating itself is rolled back by the
 *     stock bootloader; if no valid OTA slot remains at all, the bootloader
 *     falls back here.
 *
 * Recovery abilities (deliberately minimal -- reliability logic lives in
 * the main app, and this stage should almost never need to change):
 *   - Boot either OTA slot manually.
 *   - Erase NVS (clears WiFi credentials / settings -- "restore defaults").
 *     Note the boot decision itself never involves NVS; only otadata does.
 *   - Show both slots' state so field debugging doesn't need a serial cable.
 *
 * Because this stage lives in its own `factory` partition, field OTA
 * updates (which only ever write ota_0/ota_1) can never brick it.
 */
#include <cstdio>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_display_panel.hpp"
#include "lvgl.h"
#include "lvgl_v8_port.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static const char *TAG = "recovery";

// -------------------------------------------------------------------------
// OTA slot inspection
// -------------------------------------------------------------------------

struct SlotInfo {
    const esp_partition_t *partition = nullptr;
    bool has_image = false;              // partition starts with a valid app image magic
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
};

/** True if the slot both contains an app image and hasn't been marked
 *  bad by a failed OTA validation. */
static bool slot_bootable(const SlotInfo &slot)
{
    return slot.has_image &&
           slot.ota_state != ESP_OTA_IMG_INVALID &&
           slot.ota_state != ESP_OTA_IMG_ABORTED;
}

static SlotInfo inspect_slot(esp_partition_subtype_t subtype)
{
    SlotInfo info;
    info.partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
    if (info.partition == nullptr) {
        return info;
    }

    // An erased/blank slot reads back 0xFF; a flashed app image always
    // starts with the ESP image magic byte (0xE9).
    uint8_t first_byte = 0;
    if (esp_partition_read(info.partition, 0, &first_byte, 1) == ESP_OK) {
        info.has_image = (first_byte == 0xE9);
    }

    // May fail for slots with no otadata entry (e.g. dev images flashed
    // over serial) -- treat that as "no state recorded", which is fine.
    esp_ota_get_state_partition(info.partition, &info.ota_state);
    return info;
}

static const char *slot_state_text(const SlotInfo &slot)
{
    if (slot.partition == nullptr) {
        return "missing";
    }
    if (!slot.has_image) {
        return "empty";
    }
    switch (slot.ota_state) {
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_UNDEFINED:      return "present (no OTA state)";
    case ESP_OTA_IMG_NEW:            return "new (not yet booted)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verification";
    case ESP_OTA_IMG_INVALID:        return "INVALID (failed self-test)";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED (failed self-test)";
    default:                         return "unknown";
    }
}

// -------------------------------------------------------------------------
// Boot actions
// -------------------------------------------------------------------------

static lv_obj_t *s_status_label = nullptr;

static void set_status(const char *text)
{
    if (s_status_label != nullptr) {
        lv_label_set_text(s_status_label, text);
        lv_refr_now(nullptr);
    }
}

/** Points the OTA boot selector at `part` and restarts. With rollback
 *  enabled this marks the image "new": it must validate itself after boot
 *  (see firmware/app/main/boot_health.cpp) or the bootloader will bring us
 *  back here / to the other slot. */
static void boot_partition(const esp_partition_t *part)
{
    ESP_LOGI(TAG, "Booting into '%s'...", part->label);
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        set_status("Error: could not select boot partition");
        return;
    }
    set_status("Starting application...");
    vTaskDelay(pdMS_TO_TICKS(150)); // let the label render / logs flush
    esp_restart();
}

// -------------------------------------------------------------------------
// UI
// -------------------------------------------------------------------------

// Auto-boot countdown, driven by an LVGL timer. Any touch on the screen
// cancels it (leaving the recovery menu open), covering the "I forced
// recovery on purpose, don't boot past it" case without needing any state
// shared with the main app.
static constexpr int AUTO_BOOT_SECONDS = 5;
static int s_countdown_remaining = AUTO_BOOT_SECONDS;
static lv_timer_t *s_countdown_timer = nullptr;
static lv_obj_t *s_countdown_label = nullptr;
static const esp_partition_t *s_auto_boot_target = nullptr;

static void cancel_countdown()
{
    if (s_countdown_timer != nullptr) {
        lv_timer_del(s_countdown_timer);
        s_countdown_timer = nullptr;
    }
    if (s_countdown_label != nullptr) {
        lv_label_set_text(s_countdown_label, "Auto-boot cancelled -- recovery menu active");
    }
}

static void countdown_tick_cb(lv_timer_t *timer)
{
    s_countdown_remaining--;
    if (s_countdown_remaining <= 0) {
        lv_timer_del(s_countdown_timer);
        s_countdown_timer = nullptr;
        boot_partition(s_auto_boot_target);
        return;
    }
    lv_label_set_text_fmt(s_countdown_label,
                          "Booting main app in %d s -- tap anywhere for recovery menu",
                          s_countdown_remaining);
}

static void screen_touched_cb(lv_event_t *e)
{
    cancel_countdown();
}

static void boot_slot_btn_cb(lv_event_t *e)
{
    cancel_countdown();
    auto *part = static_cast<const esp_partition_t *>(lv_event_get_user_data(e));
    boot_partition(part);
}

static void erase_nvs_btn_cb(lv_event_t *e)
{
    cancel_countdown();
    ESP_LOGW(TAG, "Erasing NVS (settings / WiFi credentials)");
    esp_err_t err = nvs_flash_erase();
    set_status(err == ESP_OK ? "Settings erased (WiFi credentials cleared)"
                             : "NVS erase failed!");
}

/** One row in the recovery menu: a button that boots `slot`, plus its
 *  current state so a field technician can see what's wrong at a glance. */
static void add_slot_row(lv_obj_t *parent, const char *name, const SlotInfo &slot)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text_fmt(label, "%s: %s", name, slot_state_text(slot));
    lv_obj_set_style_text_color(label, lv_color_hex(0x9aa4b2), 0);

    if (slot_bootable(slot)) {
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 140, 44);
        lv_obj_add_event_cb(btn, boot_slot_btn_cb, LV_EVENT_CLICKED,
                            const_cast<esp_partition_t *>(slot.partition));
        lv_obj_t *btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, "Boot");
        lv_obj_center(btn_label);
    }
}

static void build_recovery_ui(const SlotInfo &slot_a, const SlotInfo &slot_b)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101317), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // A tap on empty screen space cancels the auto-boot countdown. (LVGL
    // events don't bubble by default, so the card gets its own handler
    // below, and every button's callback also cancels it explicitly.)
    lv_obj_add_event_cb(scr, screen_touched_cb, LV_EVENT_PRESSED, nullptr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Recovery");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Factory / rescue firmware -- field updates never overwrite this");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9aa4b2), 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    // --- Slot status + boot buttons -----------------------------------
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, screen_touched_cb, LV_EVENT_PRESSED, nullptr);

    add_slot_row(card, "Slot A (ota_0)", slot_a);
    add_slot_row(card, "Slot B (ota_1)", slot_b);

    // --- Erase settings -------------------------------------------------
    lv_obj_t *erase_btn = lv_btn_create(scr);
    lv_obj_set_size(erase_btn, 320, 52);
    lv_obj_align_to(erase_btn, card, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
    lv_obj_set_style_bg_color(erase_btn, lv_color_hex(0x8b2d2d), 0);
    lv_obj_add_event_cb(erase_btn, erase_nvs_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *erase_label = lv_label_create(erase_btn);
    lv_label_set_text(erase_label, "Erase settings (WiFi etc.)");
    lv_obj_center(erase_label);

    // --- Countdown / status ---------------------------------------------
    s_countdown_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_countdown_label, lv_color_hex(0xf0a500), 0);
    lv_obj_align(s_countdown_label, LV_ALIGN_BOTTOM_MID, 0, -46);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xf0a500), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    // Prefer slot A; fall back to slot B. If neither is bootable, stay in
    // the menu -- the countdown never starts and the slot rows explain why.
    if (slot_bootable(slot_a)) {
        s_auto_boot_target = slot_a.partition;
    } else if (slot_bootable(slot_b)) {
        s_auto_boot_target = slot_b.partition;
    }

    if (s_auto_boot_target != nullptr) {
        s_countdown_remaining = AUTO_BOOT_SECONDS;
        lv_label_set_text_fmt(s_countdown_label,
                              "Booting main app in %d s -- tap anywhere for recovery menu",
                              s_countdown_remaining);
        s_countdown_timer = lv_timer_create(countdown_tick_cb, 1000, nullptr);
    } else {
        lv_label_set_text(s_countdown_label,
                           "No bootable application found -- flash one over USB");
    }
}

// -------------------------------------------------------------------------
// Board bring-up
// -------------------------------------------------------------------------

// board->begin() toggles the LCD backlight/reset lines over I2C, which
// briefly spikes current draw. On a marginal 5V supply (e.g. a weak USB-A
// port), that spike can sag the rail enough to glitch the I2C write and
// fail begin() -- retrying with a short settle delay recovers from a
// one-off transient without masking a truly dead board.
static bool begin_board_with_retries(Board *b, int max_attempts = 3)
{
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "board->begin() failed (attempt %d/%d), retrying after settle delay...",
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
    ESP_LOGI(TAG, "Recovery app starting");

    // Inspect the OTA slots before any UI exists, so their state can be
    // shown even if the display bring-up ends up being the broken part.
    SlotInfo slot_a = inspect_slot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    SlotInfo slot_b = inspect_slot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    ESP_LOGI(TAG, "Slot A (ota_0): %s", slot_state_text(slot_a));
    ESP_LOGI(TAG, "Slot B (ota_1): %s", slot_state_text(slot_b));

    Board *board = new Board();
    board->init();
    assert(begin_board_with_retries(board));

    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    lvgl_port_lock(-1);
    build_recovery_ui(slot_a, slot_b);
    lvgl_port_unlock();

    // All real work happens in the LVGL task started by lvgl_port_init().
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
