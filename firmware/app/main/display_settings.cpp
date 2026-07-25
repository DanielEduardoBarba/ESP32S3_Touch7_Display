#include "display_settings.h"

#include <algorithm>

#include "esp_display_panel.hpp"
#include "esp_log.h"

#include "user_store.h"

namespace display_settings {
namespace {

const char *TAG = "display";

constexpr const char *STORE_KEY = "brightness";
constexpr uint8_t STORE_VERSION = 1;

esp_panel::drivers::Backlight *s_backlight = nullptr;
uint8_t s_brightness = BRIGHTNESS_DEFAULT;

uint8_t clampPercent(uint8_t percent)
{
    return std::max(BRIGHTNESS_MIN, std::min(BRIGHTNESS_MAX, percent));
}

void apply()
{
    if (s_backlight == nullptr) {
        return;
    }
    s_backlight->setBrightness(static_cast<int>(s_brightness));
}

} // namespace

void init(esp_panel::drivers::Backlight *backlight)
{
    s_backlight = backlight;

    uint8_t version = 0;
    uint8_t stored = 0;
    if (user_store::getU8(STORE_KEY, &version, &stored) && version == STORE_VERSION) {
        s_brightness = clampPercent(stored);
        ESP_LOGI(TAG, "Restored brightness %u%% from NVS", (unsigned)s_brightness);
    } else {
        s_brightness = BRIGHTNESS_DEFAULT;
    }
    apply();
}

uint8_t brightness()
{
    return s_brightness;
}

void setBrightness(uint8_t percent, bool persist)
{
    s_brightness = clampPercent(percent);
    apply();
    if (persist && !user_store::putU8(STORE_KEY, STORE_VERSION, s_brightness)) {
        ESP_LOGE(TAG, "FAILED to persist brightness to NVS");
    }
}

} // namespace display_settings
