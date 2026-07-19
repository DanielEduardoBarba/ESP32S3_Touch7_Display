#include "boot_health.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace boot_health {
namespace {

const char *TAG = "boot_health";

// The BOOT button on both Waveshare board variants is wired to GPIO0.
// NOTE: GPIO0 doubles as an RGB LCD data line once the panel starts, so the
// button can only be sampled here, before board/display initialization.
constexpr gpio_num_t RECOVERY_BUTTON_GPIO = GPIO_NUM_0;

// How long the button must be continuously held to trigger recovery.
// Debounces accidental taps and gives the user time to get their finger on
// the button after releasing RESET.
constexpr int HOLD_MS = 500;
constexpr int POLL_MS = 50;

} // namespace

void checkRecoveryButtonAtBoot()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << RECOVERY_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE; // button shorts the pin to GND
    gpio_config(&cfg);

    // Require the button to be held LOW for the full window; any release
    // aborts. This costs at most HOLD_MS of boot time only while the button
    // is actually being held -- the common case (not held) exits on the
    // very first sample.
    for (int held_ms = 0; held_ms < HOLD_MS; held_ms += POLL_MS) {
        if (gpio_get_level(RECOVERY_BUTTON_GPIO) != 0) {
            gpio_reset_pin(RECOVERY_BUTTON_GPIO); // release for the LCD driver
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    ESP_LOGW(TAG, "BOOT button held -- rebooting into the factory/recovery app");
    const esp_partition_t *factory = esp_partition_find_first(
                                          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (factory == nullptr) {
        ESP_LOGE(TAG, "No factory partition found; continuing normal boot");
        gpio_reset_pin(RECOVERY_BUTTON_GPIO);
        return;
    }
    if (esp_ota_set_boot_partition(factory) != ESP_OK) {
        ESP_LOGE(TAG, "Could not select factory partition; continuing normal boot");
        gpio_reset_pin(RECOVERY_BUTTON_GPIO);
        return;
    }
    esp_restart();
}

void commitRunningImageIfPending()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        // Typical for images flashed over serial during development (no OTA
        // state entry exists) -- nothing to commit.
        ESP_LOGI(TAG, "Running from '%s' (no OTA state tracked)", running->label);
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        // We made it through full subsystem bring-up without crashing:
        // that's the self-test. Locking the image in prevents the
        // bootloader from rolling back on the next reset.
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image in '%s' passed self-test -- marked valid, rollback cancelled",
                     running->label);
        } else {
            ESP_LOGE(TAG, "Failed to mark image valid: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "Running from '%s' (state %d, already validated)", running->label, (int)state);
    }
}

} // namespace boot_health
