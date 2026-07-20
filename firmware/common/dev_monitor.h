#pragma once

/**
 * Combined development overlay card: FPS | CPU | RAM | PSRAM in ONE small
 * panel (bottom-right, LVGL sys layer, on top of every scene). Shared by
 * the app and the factory stage (header-only on purpose -- no extra build
 * wiring for either ESP-IDF project).
 *
 * Only exists in DEV builds: APP_DEV_MODE (see app_config.h) defaults to 1
 * and is forced to 0 by `./build.sh --prod`, which compiles the whole
 * overlay out. Replaces LVGL's built-in perf monitor (disabled in
 * sdkconfig) so everything lives in one card.
 *
 *   FPS: frames counted via the display driver's monitor_cb hook.
 *   CPU: 100 - lv_timer_get_idle() (same source LVGL's own monitor used).
 *   RAM/PSRAM: used/total + percent from heap_caps (internal vs SPIRAM).
 */

#include <atomic>
#include <cstdint>

#include "esp_heap_caps.h"
#include "lvgl.h"

// Self-contained on purpose: APP_DEV_MODE comes from app_config.h, and
// relying on the includER to have pulled it in first silently compiled the
// overlay OUT wherever it hadn't been (undefined macro == 0 in #if).
#include "../app/main/app_config.h"

namespace dev_monitor {

#if APP_DEV_MODE

namespace detail {
inline std::atomic<uint32_t> s_frames{0};

inline void flushMonitorCb(lv_disp_drv_t *, uint32_t, uint32_t)
{
    s_frames.fetch_add(1, std::memory_order_relaxed);
}

inline void refreshTimerCb(lv_timer_t *t)
{
    auto *label = static_cast<lv_obj_t *>(t->user_data);

    uint32_t fps = s_frames.exchange(0, std::memory_order_relaxed);
    uint32_t cpu = 100 - lv_timer_get_idle();

    size_t free_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t total_i = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t total_p = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned used_i_pct = total_i > 0 ? (unsigned)(100 * (total_i - free_i) / total_i) : 0;
    unsigned used_p_pct = total_p > 0 ? (unsigned)(100 * (total_p - free_p) / total_p) : 0;

    lv_label_set_text_fmt(label,
                          "%u FPS  CPU %u%%\n"
                          "RAM %u/%u KB (%u%%)\n"
                          "PSRAM %u/%u KB (%u%%)",
                          (unsigned)fps, (unsigned)cpu,
                          (unsigned)((total_i - free_i) / 1024), (unsigned)(total_i / 1024),
                          used_i_pct,
                          (unsigned)((total_p - free_p) / 1024), (unsigned)(total_p / 1024),
                          used_p_pct);
}
} // namespace detail

/** Builds the overlay. Call once after LVGL is up (with the LVGL lock held). */
inline void show()
{
    // Count rendered frames via the (otherwise unused) driver hook.
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != nullptr && disp->driver != nullptr) {
        disp->driver->monitor_cb = detail::flushMonitorCb;
    }

    lv_obj_t *label = lv_label_create(lv_layer_sys());
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_60, 0);
    lv_obj_set_style_radius(label, 6, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(label, "...");
    lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

    lv_timer_create(detail::refreshTimerCb, 1000, label);
}

#else // production build: compiled out entirely

inline void show() {}

#endif // APP_DEV_MODE

} // namespace dev_monitor
