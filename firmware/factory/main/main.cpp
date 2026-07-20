/*
 * FACTORY / RECOVERY APP
 * ----------------------
 * The small "rescue firmware" of the high-reliability boot architecture.
 * Boot flow (all standard ESP-IDF -- ROM bootloader -> stock 2nd-stage
 * bootloader -> otadata slot selection -> this app or an OTA slot):
 *
 *   - EVERY normal boot: the app, once it proves healthy, points otadata
 *     back HERE (see firmware/app/main/boot_health.cpp), so each reset runs
 *
 *       ROM -> bootloader -> factory [splash, APP_SPLASH_SECONDS;
 *                                     triple-tap = recovery menu]
 *           -> bootloader -> app (the slot picked by pick_auto_boot_target)
 *
 *     A corrupted/crashing app never reaches the handoff code, so it can
 *     never redirect the chain -- stock rollback / the factory fallback
 *     still catch it.
 *
 *   - Fresh flash / factory reset: otadata is empty, so the bootloader runs
 *     this app directly; same splash -> auto-boot behavior.
 *
 *   - Entering the recovery menu: tap the splash's logo/text
 *     APP_SPLASH_TAPS_FOR_MENU times (rapidly) during the splash. (NOTE:
 *     holding BOOT at POWER-ON is taken by Espressif's ROM download mode
 *     and never reaches this app -- but holding BOOT while the MAIN app
 *     starts still reboots into here, see firmware/app/main/boot_health.cpp.)
 *
 *   - Automatic recovery: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an
 *     OTA image that crashes before validating itself is rolled back by the
 *     stock bootloader; if no valid OTA slot remains at all, the bootloader
 *     falls back here (and with nothing bootable the menu opens directly).
 *
 * Recovery abilities (deliberately minimal -- reliability logic lives in
 * the main app, and this stage should almost never need to change):
 *   - Point otadata at EITHER OTA slot and boot straight into it.
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

// Shared app-level config: splash colors/text/timing live with the rest of
// the product's tunables so branding is changed in ONE place.
#include "../../app/main/app_config.h"
// Combined FPS/CPU/RAM/PSRAM overlay (dev builds only).
#include "../../common/dev_monitor.h"

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
    char version[34] = "";               // embedded app version ("?" when unreadable)
};

/** True if the slot contains an app image at all -- the menu lets the user
 *  boot ANY image, including one marked INVALID by a rollback (often just a
 *  reset that happened before the app's self-test committed; see
 *  boot_partition(), which clears the stale mark first). */
static bool slot_bootable(const SlotInfo &slot)
{
    return slot.has_image;
}

/** Stricter check for AUTOMATIC booting: never auto-boot a slot the
 *  rollback machinery flagged -- that's a job for an explicit user choice. */
static bool slot_auto_bootable(const SlotInfo &slot)
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

    // The version stamped into the image's app descriptor (PROJECT_VER =
    // APP_VERSION from app_config.h) -- shows WHICH build lives in the slot.
    esp_app_desc_t desc;
    if (info.has_image &&
        esp_ota_get_partition_description(info.partition, &desc) == ESP_OK) {
        snprintf(info.version, sizeof(info.version), "v%s", desc.version);
    } else if (info.has_image) {
        snprintf(info.version, sizeof(info.version), "v?");
    }
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
    // The factory-first boot chain erases otadata on every healthy boot
    // (arming the factory clears all slot state records), so "no state" is
    // the NORMAL condition here -- not a problem with the image.
    case ESP_OTA_IMG_UNDEFINED:      return "present";
    case ESP_OTA_IMG_NEW:            return "new (not yet booted)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verification";
    // Usually just a reset before the app's self-test committed -- booting
    // it again from here clears the mark and gives it another chance.
    case ESP_OTA_IMG_INVALID:        return "marked invalid (early reset?) -- boot to retry";
    case ESP_OTA_IMG_ABORTED:        return "marked aborted -- boot to retry";
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
    // Two-step selection: selecting the FACTORY partition first erases
    // otadata entirely, which clears any stale INVALID/ABORTED mark left by
    // a rollback (e.g. a reset before the app's self-test committed).
    // Selecting the target slot then writes a clean NEW entry, so the
    // bootloader gives the image a fresh pending-verify boot instead of
    // refusing it.
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (factory != nullptr && part != factory) {
        esp_ota_set_boot_partition(factory);
    }
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

// Splash flow: show the brand (color/text/logo from app_config.h) for
// APP_SPLASH_SECONDS with NO text about what's happening, then silently
// boot the main app. Tapping the logo/text APP_SPLASH_TAPS_FOR_MENU times
// during the splash opens the recovery menu instead.
static lv_timer_t *s_splash_timer = nullptr;
static int s_splash_taps = 0;
static const esp_partition_t *s_auto_boot_target = nullptr;
static SlotInfo s_slot_a, s_slot_b;

static void build_recovery_ui(const SlotInfo &slot_a, const SlotInfo &slot_b);

static void cancel_splash_timer()
{
    if (s_splash_timer != nullptr) {
        lv_timer_del(s_splash_timer);
        s_splash_timer = nullptr;
    }
}

static void splash_timeout_cb(lv_timer_t *timer)
{
    cancel_splash_timer();
    if (s_auto_boot_target != nullptr) {
        boot_partition(s_auto_boot_target);
        return;
    }
    // Nothing bootable -- fall through to the menu so the slot rows can
    // explain what's wrong.
    ESP_LOGW(TAG, "No bootable OTA slot -- opening the recovery menu");
    lv_obj_clean(lv_scr_act());
    build_recovery_ui(s_slot_a, s_slot_b);
}

static void splash_logo_tapped_cb(lv_event_t *e)
{
    s_splash_taps++;
    ESP_LOGI(TAG, "Splash tap %d/%d", s_splash_taps, APP_SPLASH_TAPS_FOR_MENU);
    if (s_splash_taps >= APP_SPLASH_TAPS_FOR_MENU) {
        cancel_splash_timer();
        lv_obj_clean(lv_scr_act());
        build_recovery_ui(s_slot_a, s_slot_b);
    }
}

#if APP_SPLASH_SHOW_LOGO
// When APP_SPLASH_SHOW_LOGO is 1, link an LVGL image descriptor with this
// name into the factory stage (e.g. generated by LVGL's image converter).
extern "C" const lv_img_dsc_t APP_SPLASH_LOGO_IMG;
#endif

/** The "last running OTA slot" breadcrumb the main app records in NVS
 *  (see firmware/app/main/boot_health.cpp). Needed because selecting the
 *  factory partition (forced recovery) ERASES otadata -- this is how the
 *  auto-boot still returns to the slot that was actually active. Returns
 *  the partition subtype value, or 0 when no hint exists. */
static uint8_t read_last_slot_hint()
{
    nvs_handle_t handle;
    if (nvs_open("recovery", NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    uint8_t value = 0;
    nvs_get_u8(handle, "last_slot", &value);
    nvs_close(handle);
    return value;
}

/** Auto-boot target, in order of trust:
 *    1. The slot otadata says is ACTIVE (the normal case).
 *    2. The app's last-running-slot NVS breadcrumb (otadata was erased).
 *    3. Slot A, then slot B (fresh flash: no history at all).
 *  Only bootable slots are considered; nullptr when nothing qualifies. */
static const esp_partition_t *pick_auto_boot_target()
{
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    if (configured != nullptr) {
        if (s_slot_a.partition == configured && slot_auto_bootable(s_slot_a)) {
            ESP_LOGI(TAG, "Auto-boot target: ota_0 (active per otadata)");
            return s_slot_a.partition;
        }
        if (s_slot_b.partition == configured && slot_auto_bootable(s_slot_b)) {
            ESP_LOGI(TAG, "Auto-boot target: ota_1 (active per otadata)");
            return s_slot_b.partition;
        }
    }

    uint8_t hint = read_last_slot_hint();
    if (hint == ESP_PARTITION_SUBTYPE_APP_OTA_0 && slot_auto_bootable(s_slot_a)) {
        ESP_LOGI(TAG, "Auto-boot target: ota_0 (last-running-slot hint)");
        return s_slot_a.partition;
    }
    if (hint == ESP_PARTITION_SUBTYPE_APP_OTA_1 && slot_auto_bootable(s_slot_b)) {
        ESP_LOGI(TAG, "Auto-boot target: ota_1 (last-running-slot hint)");
        return s_slot_b.partition;
    }

    if (slot_auto_bootable(s_slot_a)) {
        ESP_LOGI(TAG, "Auto-boot target: ota_0 (default preference)");
        return s_slot_a.partition;
    }
    if (slot_auto_bootable(s_slot_b)) {
        ESP_LOGI(TAG, "Auto-boot target: ota_1 (default preference)");
        return s_slot_b.partition;
    }
    return nullptr;
}

static void build_splash_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(APP_SPLASH_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Generous invisible tap target around the brand mark, so the triple
    // tap doesn't demand pixel accuracy.
    lv_obj_t *tap_area = lv_obj_create(scr);
    lv_obj_set_size(tap_area, 360, 220);
    lv_obj_center(tap_area);
    lv_obj_set_style_bg_opa(tap_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area, 0, 0);
    lv_obj_clear_flag(tap_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tap_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap_area, splash_logo_tapped_cb, LV_EVENT_CLICKED, nullptr);

#if APP_SPLASH_SHOW_LOGO
    lv_obj_t *logo = lv_img_create(tap_area);
    lv_img_set_src(logo, &APP_SPLASH_LOGO_IMG);
    lv_obj_center(logo);
#else
    lv_obj_t *text = lv_label_create(tap_area);
    lv_label_set_text(text, APP_SPLASH_TEXT);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(APP_SPLASH_TEXT_COLOR), 0);
    lv_obj_center(text);
#endif

    // Prefer whatever otadata already points at; fall back to the app's
    // last-running-slot breadcrumb, then slot A/B (see pick_auto_boot_target).
    s_auto_boot_target = pick_auto_boot_target();

    s_splash_timer = lv_timer_create(splash_timeout_cb, APP_SPLASH_SECONDS * 1000, nullptr);
}

static void boot_slot_btn_cb(lv_event_t *e)
{
    // "Point otadata at this slot and go": boot_partition() writes the
    // boot selector (esp_ota_set_boot_partition) and restarts, so this is
    // both "switch which OTA is active" AND "enter it" in one press.
    auto *part = static_cast<const esp_partition_t *>(lv_event_get_user_data(e));
    boot_partition(part);
}

static void erase_nvs_btn_cb(lv_event_t *e)
{
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
    if (slot.version[0] != '\0') {
        lv_label_set_text_fmt(label, "%s: %s  --  %s", name, slot.version, slot_state_text(slot));
    } else {
        lv_label_set_text_fmt(label, "%s: %s", name, slot_state_text(slot));
    }
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

    // --- Status ----------------------------------------------------------
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label,
                      "Booting a slot points otadata at it -- the device enters that app now\n"
                      "and keeps booting it until changed again.");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xf0a500), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);
}

// -------------------------------------------------------------------------
// Board bring-up
// -------------------------------------------------------------------------

/** One-shot flag left by the app's splash triple-tap / BOOT-button flow
 *  (see firmware/app/main/boot_health.cpp): when set, skip the factory
 *  splash and open the recovery MENU directly. */
static bool consume_recovery_menu_flag()
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open("recovery", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    bool set = nvs_get_u8(handle, "menu", &value) == ESP_OK && value != 0;
    if (set) {
        nvs_erase_key(handle, "menu");
        nvs_commit(handle);
    }
    nvs_close(handle);
    return set;
}

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
    s_slot_a = inspect_slot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    s_slot_b = inspect_slot(ESP_PARTITION_SUBTYPE_APP_OTA_1);
    ESP_LOGI(TAG, "Slot A (ota_0): %s", slot_state_text(s_slot_a));
    ESP_LOGI(TAG, "Slot B (ota_1): %s", slot_state_text(s_slot_b));

    Board *board = new Board();
    board->init();
    assert(begin_board_with_retries(board));

    // Kill the "white flash": the panel shows garbage/white between the
    // backlight coming up (inside begin()) and the first real LVGL frame.
    auto *backlight = board->getBacklight();
    if (backlight != nullptr) {
        backlight->off();
    }

    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    // Open the menu directly when the app's BOOT-button flow asked for it;
    // otherwise show the branded splash and auto-boot.
    bool menu_requested = consume_recovery_menu_flag();
    lvgl_port_lock(-1);
    if (menu_requested) {
        ESP_LOGI(TAG, "Recovery menu requested by the app -- skipping splash");
        build_recovery_ui(s_slot_a, s_slot_b);
    } else {
        build_splash_ui();
    }
    dev_monitor::show(); // FPS/CPU/RAM/PSRAM card (dev builds only)
    lv_refr_now(nullptr); // first frame is on screen before the backlight returns
    lvgl_port_unlock();
    if (backlight != nullptr) {
        backlight->on();
    }

    // All real work happens in the LVGL task started by lvgl_port_init().
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
