/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 *
 * Simplified LVGL v8 porting layer for RGB LCDs with tearing-avoidance
 * disabled (this project doesn't need it at 800x480/16bpp with a modest
 * refresh rate). Adapted from esp-arduino-libs/ESP32_Display_Panel's
 * official ESP-IDF lvgl_v8_port example, trimmed to only the code paths
 * actually used by this board (RGB parallel bus, no rotation).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#undef ESP_UTILS_LOG_TAG
#define ESP_UTILS_LOG_TAG "LvPort"
#include "esp_lib_utils.h"
#include "lvgl_v8_port.h"

using namespace esp_panel::drivers;

static SemaphoreHandle_t lvgl_mux = nullptr;
static TaskHandle_t lvgl_task_handle = nullptr;
static esp_timer_handle_t lvgl_tick_timer = NULL;
static void *lvgl_buf[LVGL_PORT_BUFFER_NUM] = {};

static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    LCD *lcd = (LCD *)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    lcd->drawBitmap(offsetx1, offsety1, offsetx2 - offsetx1 + 1, offsety2 - offsety1 + 1, (const uint8_t *)color_map);
    /* For RGB LCD, the panel driver copies the buffer synchronously, so we can
     * notify LVGL immediately instead of waiting for a "transfer done" ISR. */
    if (lcd->getBus()->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lv_disp_flush_ready(drv);
    }
}

static bool onDrawBitmapFinishCallback(void *user_data)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_data;
    lv_disp_flush_ready(drv);
    return false;
}

static void rounder_callback(lv_disp_drv_t *drv, lv_area_t *area)
{
    LCD *lcd = (LCD *)drv->user_data;
    uint8_t x_align = lcd->getBasicAttributes().basic_bus_spec.x_coord_align;
    uint8_t y_align = lcd->getBasicAttributes().basic_bus_spec.y_coord_align;

    if (x_align > 1) {
        area->x1 &= ~(x_align - 1);
        area->x2 = (area->x2 & ~(x_align - 1)) + x_align - 1;
    }
    if (y_align > 1) {
        area->y1 &= ~(y_align - 1);
        area->y2 = (area->y2 & ~(y_align - 1)) + y_align - 1;
    }
}

static lv_disp_t *display_init(LCD *lcd)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, nullptr, "Invalid LCD device");
    ESP_UTILS_CHECK_FALSE_RETURN(lcd->getRefreshPanelHandle() != nullptr, nullptr, "LCD device is not initialized");

    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    auto lcd_width = lcd->getFrameWidth();
    auto lcd_height = lcd->getFrameHeight();
    int buffer_size = lcd_width * LVGL_PORT_BUFFER_SIZE_HEIGHT;

    for (int i = 0; i < LVGL_PORT_BUFFER_NUM; i++) {
        lvgl_buf[i] = heap_caps_malloc(buffer_size * sizeof(lv_color_t), LVGL_PORT_BUFFER_MALLOC_CAPS);
        ESP_UTILS_CHECK_NULL_RETURN(lvgl_buf[i], nullptr, "Malloc LVGL buffer failed");
    }

    lv_disp_draw_buf_init(&disp_buf, lvgl_buf[0], lvgl_buf[1], buffer_size);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = lcd_width;
    disp_drv.ver_res = lcd_height;
    disp_drv.flush_cb = flush_callback;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = (void *)lcd;
    if ((lcd->getBasicAttributes().basic_bus_spec.x_coord_align > 1) ||
            (lcd->getBasicAttributes().basic_bus_spec.y_coord_align > 1)) {
        disp_drv.rounder_cb = rounder_callback;
    }

    if (lcd->getBus()->getBasicAttributes().type != ESP_PANEL_BUS_TYPE_RGB) {
        lcd->attachDrawBitmapFinishCallback(onDrawBitmapFinishCallback, (void *)&disp_drv);
    }

    return lv_disp_drv_register(&disp_drv);
}

static SemaphoreHandle_t touch_detected;

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    Touch *tp = (Touch *)indev_drv->user_data;
    TouchPoint point;
    data->state = LV_INDEV_STATE_RELEASED;

    if (tp->isInterruptEnabled() && (xSemaphoreTake(touch_detected, 0) == pdFALSE)) {
        return;
    }

    int read_touch_result = tp->readPoints(&point, 1, 0);
    if (read_touch_result > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static bool onTouchInterruptCallback(void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(touch_detected, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return false;
}

static lv_indev_t *indev_init(Touch *tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(tp != nullptr, nullptr, "Invalid touch device");
    ESP_UTILS_CHECK_FALSE_RETURN(tp->getPanelHandle() != nullptr, nullptr, "Touch device is not initialized");

    static lv_indev_drv_t indev_drv_tp;

    if (tp->isInterruptEnabled()) {
        touch_detected = xSemaphoreCreateBinary();
        tp->attachInterruptCallback(onTouchInterruptCallback, tp);
    }

    lv_indev_drv_init(&indev_drv_tp);
    indev_drv_tp.type = LV_INDEV_TYPE_POINTER;
    indev_drv_tp.read_cb = touchpad_read;
    indev_drv_tp.user_data = (void *)tp;

    return lv_indev_drv_register(&indev_drv_tp);
}

#if !LV_TICK_CUSTOM
static void tick_increment(void *arg)
{
    lv_tick_inc(LVGL_PORT_TICK_PERIOD_MS);
}

static bool tick_init(void)
{
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &tick_increment,
        .name = "LVGL tick"
    };
    ESP_UTILS_CHECK_ERROR_RETURN(
        esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer), false, "Create LVGL tick timer failed"
    );
    ESP_UTILS_CHECK_ERROR_RETURN(
        esp_timer_start_periodic(lvgl_tick_timer, LVGL_PORT_TICK_PERIOD_MS * 1000), false,
        "Start LVGL tick timer failed"
    );

    return true;
}

static bool tick_deinit(void)
{
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_stop(lvgl_tick_timer), false, "Stop LVGL tick timer failed");
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_delete(lvgl_tick_timer), false, "Delete LVGL tick timer failed");
    return true;
}
#endif

static void lvgl_port_task(void *arg)
{
    ESP_UTILS_LOGD("Starting LVGL task");

    uint32_t task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_port_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (task_delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

bool lvgl_port_init(LCD *lcd, Touch *tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, false, "Invalid LCD device");

    lv_disp_t *disp = nullptr;
    lv_indev_t *indev = nullptr;

    lv_init();
#if !LV_TICK_CUSTOM
    ESP_UTILS_CHECK_FALSE_RETURN(tick_init(), false, "Initialize LVGL tick failed");
#endif

    ESP_UTILS_LOGI("Initializing LVGL display driver");
    disp = display_init(lcd);
    ESP_UTILS_CHECK_NULL_RETURN(disp, false, "Initialize LVGL display driver failed");

    if (tp != nullptr) {
        ESP_UTILS_LOGD("Initialize LVGL input driver");
        indev = indev_init(tp);
        ESP_UTILS_CHECK_NULL_RETURN(indev, false, "Initialize LVGL input driver failed");
    }

    ESP_UTILS_LOGD("Create mutex for LVGL");
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "Create LVGL mutex failed");

    ESP_UTILS_LOGD("Create LVGL task");
    BaseType_t core_id = (LVGL_PORT_TASK_CORE < 0) ? tskNO_AFFINITY : LVGL_PORT_TASK_CORE;
    BaseType_t ret = xTaskCreatePinnedToCore(
                         lvgl_port_task, "lvgl", LVGL_PORT_TASK_STACK_SIZE, NULL,
                         LVGL_PORT_TASK_PRIORITY, &lvgl_task_handle, core_id
                     );
    ESP_UTILS_CHECK_FALSE_RETURN(ret == pdPASS, false, "Create LVGL task failed");

    return true;
}

bool lvgl_port_deinit(void)
{
#if !LV_TICK_CUSTOM
    ESP_UTILS_CHECK_FALSE_RETURN(tick_deinit(), false, "Deinitialize LVGL tick failed");
#endif

    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_lock(-1), false, "Lock LVGL failed");
    if (lvgl_task_handle != nullptr) {
        vTaskDelete(lvgl_task_handle);
        lvgl_task_handle = nullptr;
    }
    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_unlock(), false, "Unlock LVGL failed");

    for (int i = 0; i < LVGL_PORT_BUFFER_NUM; i++) {
        if (lvgl_buf[i] != nullptr) {
            free(lvgl_buf[i]);
            lvgl_buf[i] = nullptr;
        }
    }
    if (lvgl_mux != nullptr) {
        vSemaphoreDelete(lvgl_mux);
        lvgl_mux = nullptr;
    }

    return true;
}

bool lvgl_port_lock(int timeout_ms)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE);
}

bool lvgl_port_unlock(void)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");
    xSemaphoreGiveRecursive(lvgl_mux);
    return true;
}
