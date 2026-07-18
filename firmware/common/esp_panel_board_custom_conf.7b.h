/*
 * Custom ESP32_Display_Panel board configuration for the Waveshare
 * ESP32-S3-Touch-LCD-7B (1024x600).
 *
 * SOURCE OF TRUTH: every hardware value below is taken directly from
 * Waveshare's OFFICIAL demo repository for this exact board:
 *   https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B
 * (examples/Arduino/examples/13_LVGL_TRANSPLANT/{rgb_lcd_port.h,rgb_lcd_port.cpp,
 *  gt911.h,i2c.h,io_extension.h}) -- NOT guessed, unlike earlier revisions of
 * this file.
 *
 * KEY FACTS ABOUT THE 7B (all different from what you might assume):
 * - The panel is PURE RGB: no init-command controller, no "3-wire SPI"
 *   interface at all. (An earlier revision of this file wrongly assumed
 *   ST7701 based on a copy-paste error in Waveshare's own wiki FAQ -- a
 *   1024x600 panel physically can't be an ST7701 anyway, that IC maxes out
 *   at 480x864.) We use the library's ST7262 driver, which -- exactly as on
 *   the original "7" -- is just a generic RGB refresh driver.
 * - The 7B does NOT have a CH422G IO expander. It has Waveshare's own
 *   custom "IO EXTENSION" chip at I2C address 0x24 (registers: 0x02=pin
 *   mode, 0x03=IO output bitmask, 0x04=IO input, 0x05=backlight PWM,
 *   0x06=ADC/battery; pins: IO1=TP_RST, IO2=BL, IO3=LCD_RST, IO4=SD_CS,
 *   IO5=USB/CAN sel). The esp32_io_expander library has NO driver for it.
 *   This was confirmed empirically: an I2C bus scan on a real 7B found
 *   ONLY address 0x24 (a CH422G would answer on 0x23/0x24/0x26/0x38).
 * - Waveshare's demo needs the IO EXTENSION chip for exactly ONE part of
 *   display/touch bring-up: resetting the GT911 (IO_1 = TP_RST) with a
 *   specific timing dance on the INT pin (selects I2C address 0x5D). That
 *   sequence is replicated verbatim in ESP_PANEL_BOARD_TOUCH_PRE_BEGIN_FUNCTION
 *   at the bottom of this file via raw I2C writes. Everything else is
 *   unused for bring-up: the LCD is never reset (LCD_RST unused by the
 *   demo) and wavesahre_rgb_lcd_bl_on()/off() are literally EMPTY functions
 *   -- the backlight is on by default in hardware. So this config disables
 *   the library's expander and backlight drivers entirely. (SD-CS /
 *   CAN-select / PWM dimming via the IO EXTENSION chip can be added later
 *   in app code if needed -- see the io_extension.h register map above.)
 * - Panel timing (from rgb_lcd_port.h/.cpp): 1024x600 @ PCLK 30MHz,
 *   HPW=162 HBP=152 HFP=48, VPW=45 VBP=13 VFP=3, PCLK active-negative.
 * - RGB data/sync GPIOs are identical to the original "7".
 * - GT911 touch: I2C0 (SCL=9, SDA=8), INT=GPIO4, RST not connected.
 */

#pragma once

// Needed by ESP_PANEL_BOARD_TOUCH_PRE_BEGIN_FUNCTION below (raw I2C writes
// to the IO EXTENSION chip + GPIO control of the touch INT pin).
#include "driver/i2c.h"
#include "driver/gpio.h"

// *INDENT-OFF*

/**
 * @brief Flag to enable custom board configuration (0/1)
 *
 * Required by ESP32_Display_Panel's file-based custom-board mechanism (see
 * its own template `esp_panel_board_custom_conf.h` at the component root):
 * everything below is compiled out unless this is 1. This is a plain C
 * macro, NOT a Kconfig symbol -- unrelated to (and takes priority over) the
 * `CONFIG_ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM` Kconfig choice, which only
 * matters for the *menuconfig-driven* custom-board path (not used here).
 */
#define ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM  (1)

#if ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Please update the following macros to configure general panel /////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_NAME                "Waveshare:ESP32_S3_TOUCH_LCD_7B"

// From Waveshare's official demo (rgb_lcd_port.h: EXAMPLE_LCD_H_RES/V_RES).
#define ESP_PANEL_BOARD_WIDTH               (1024)
#define ESP_PANEL_BOARD_HEIGHT              (600)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Please update the following macros to configure the LCD panel /////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_LCD             (1)

#if ESP_PANEL_BOARD_USE_LCD
// Generic RGB refresh driver (same as the original "7") -- the 7B panel is
// pure RGB with no init-command interface. See file header.
#define ESP_PANEL_BOARD_LCD_CONTROLLER      ST7262

#define ESP_PANEL_BOARD_LCD_BUS_TYPE        (ESP_PANEL_BUS_TYPE_RGB)

#if ESP_PANEL_BOARD_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB

    // Pure RGB interface -- no "3-wire SPI" control panel on this board.
    #define ESP_PANEL_BOARD_LCD_RGB_USE_CONTROL_PANEL       (0)

    /* For refresh panel (RGB) -- timing values verbatim from Waveshare's
     * official demo (rgb_lcd_port.cpp `.timings` struct) */
    #define ESP_PANEL_BOARD_LCD_RGB_CLK_HZ          (30 * 1000 * 1000)
    #define ESP_PANEL_BOARD_LCD_RGB_HPW             (162)
    #define ESP_PANEL_BOARD_LCD_RGB_HBP             (152)
    #define ESP_PANEL_BOARD_LCD_RGB_HFP             (48)
    #define ESP_PANEL_BOARD_LCD_RGB_VPW             (45)
    #define ESP_PANEL_BOARD_LCD_RGB_VBP             (13)
    #define ESP_PANEL_BOARD_LCD_RGB_VFP             (3)
    #define ESP_PANEL_BOARD_LCD_RGB_PCLK_ACTIVE_NEG (1)     // 0: rising edge, 1: falling edge
    #define ESP_PANEL_BOARD_LCD_RGB_DATA_WIDTH      (16)
    #define ESP_PANEL_BOARD_LCD_RGB_PIXEL_BITS      (ESP_PANEL_LCD_COLOR_BITS_RGB565)
    #define ESP_PANEL_BOARD_LCD_RGB_BOUNCE_BUF_SIZE (ESP_PANEL_BOARD_WIDTH * 10)
    #define ESP_PANEL_BOARD_LCD_RGB_IO_HSYNC        (46)
    #define ESP_PANEL_BOARD_LCD_RGB_IO_VSYNC        (3)
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DE           (5)
    #define ESP_PANEL_BOARD_LCD_RGB_IO_PCLK         (7)
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DISP         (-1)

    /* Data GPIOs -- identical to the original "7", confirmed against the
     * official 7B demo (rgb_lcd_port.h EXAMPLE_LCD_IO_RGB_DATA0-15) */
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA0        (14)    // B3
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA1        (38)    // B4
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA2        (18)    // B5
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA3        (17)    // B6
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA4        (10)    // B7
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA5        (39)    // G2
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA6        (0)     // G3
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA7        (45)    // G4
#if ESP_PANEL_BOARD_LCD_RGB_DATA_WIDTH > 8
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA8        (48)    // G5
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA9        (47)    // G6
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA10       (21)    // G7
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA11       (1)     // R3
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA12       (2)     // R4
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA13       (42)    // R5
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA14       (41)    // R6
    #define ESP_PANEL_BOARD_LCD_RGB_IO_DATA15       (40)    // R7
#endif // ESP_PANEL_BOARD_LCD_RGB_DATA_WIDTH

#endif // ESP_PANEL_BOARD_LCD_BUS_TYPE

#define ESP_PANEL_BOARD_LCD_COLOR_BITS          (ESP_PANEL_LCD_COLOR_BITS_RGB888)
#define ESP_PANEL_BOARD_LCD_COLOR_BGR_ORDER     (0)     // 0: RGB, 1: BGR
#define ESP_PANEL_BOARD_LCD_COLOR_INEVRT_BIT    (0)     // 0/1

#define ESP_PANEL_BOARD_LCD_SWAP_XY             (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_X            (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_Y            (0)
#define ESP_PANEL_BOARD_LCD_GAP_X               (0)
#define ESP_PANEL_BOARD_LCD_GAP_Y               (0)

// No LCD reset line is used on this board (the official demo never resets
// the panel; EXAMPLE_LCD_IO_RST = -1).
#define ESP_PANEL_BOARD_LCD_RST_IO              (-1)
#define ESP_PANEL_BOARD_LCD_RST_LEVEL           (0)

#endif // ESP_PANEL_BOARD_USE_LCD

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Please update the following macros to configure the touch panel ///////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_TOUCH               (1)

#if ESP_PANEL_BOARD_USE_TOUCH
#define ESP_PANEL_BOARD_TOUCH_CONTROLLER        GT911

#define ESP_PANEL_BOARD_TOUCH_BUS_TYPE          (ESP_PANEL_BUS_TYPE_I2C)

#if (ESP_PANEL_BOARD_TOUCH_BUS_TYPE == ESP_PANEL_BUS_TYPE_I2C) || \
    (ESP_PANEL_BOARD_TOUCH_BUS_TYPE == ESP_PANEL_BUS_TYPE_SPI)
#define ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST        (0)
#endif

#if ESP_PANEL_BOARD_TOUCH_BUS_TYPE == ESP_PANEL_BUS_TYPE_I2C
    #define ESP_PANEL_BOARD_TOUCH_I2C_HOST_ID           (0)
#if !ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST
    #define ESP_PANEL_BOARD_TOUCH_I2C_CLK_HZ            (400 * 1000)
    #define ESP_PANEL_BOARD_TOUCH_I2C_SCL_PULLUP        (1)
    #define ESP_PANEL_BOARD_TOUCH_I2C_SDA_PULLUP        (1)
    #define ESP_PANEL_BOARD_TOUCH_I2C_IO_SCL            (9)
    #define ESP_PANEL_BOARD_TOUCH_I2C_IO_SDA            (8)
#endif
    #define ESP_PANEL_BOARD_TOUCH_I2C_ADDRESS           (0)     // 0 = use GT911 default (0x5D)
#endif // ESP_PANEL_BOARD_TOUCH_BUS_TYPE

#define ESP_PANEL_BOARD_TOUCH_SWAP_XY           (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_X          (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_Y          (0)

// The GT911's reset line goes through the custom IO EXTENSION chip (IO_1),
// so there's no plain GPIO to configure here (-1) -- the reset is done
// manually in ESP_PANEL_BOARD_TOUCH_PRE_BEGIN_FUNCTION below, replicating
// Waveshare's own demo sequence (gt911.cpp touch_gt911_init()) exactly.
#define ESP_PANEL_BOARD_TOUCH_RST_IO            (-1)
#define ESP_PANEL_BOARD_TOUCH_RST_LEVEL         (0)
#define ESP_PANEL_BOARD_TOUCH_INT_IO            (4)
#define ESP_PANEL_BOARD_TOUCH_INT_LEVEL         (0)

#endif // ESP_PANEL_BOARD_USE_TOUCH

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// Please update the following macros to configure the backlight ////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The 7B's backlight is ON by default in hardware and its control line goes
// through the custom IO EXTENSION chip (reg 0x05 = PWM dimming) which this
// library has no driver for. Waveshare's own demo's bl_on()/bl_off() are
// literally empty functions -- so we simply don't configure a backlight
// driver here. (PWM dimming can be added later in app code by writing
// {0x05, duty} to I2C device 0x24 -- see the file header.)
#define ESP_PANEL_BOARD_USE_BACKLIGHT           (0)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// Please update the following macros to configure the IO expander //////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The 7B does NOT have a CH422G (or any chip this library supports) -- it
// has Waveshare's custom "IO EXTENSION" chip at 0x24, which this config
// drives directly with raw I2C writes where needed (touch reset below).
#define ESP_PANEL_BOARD_USE_EXPANDER            (0)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////// Please utilize the following macros to execute any additional code if required /////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Pre-begin function for touch panel initialization
 *
 * Replicates Waveshare's official demo GT911 bring-up EXACTLY (see
 * examples/Arduino/examples/13_LVGL_TRANSPLANT/gt911.cpp in
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B):
 *   1. IO EXTENSION init: write {0x02, 0xFF} to 0x24 (all pins -> output)
 *   2. TP_RST (IO EXTENSION IO_1) low            -> write {0x03, 0xFD}
 *   3. wait 100ms, drive TP_INT (GPIO4) low (selects GT911 address 0x5D)
 *   4. wait 100ms, TP_RST high                   -> write {0x03, 0xFF}
 *   5. wait 200ms for the GT911 to boot, then release TP_INT
 *
 * The I2C host isn't installed by the library yet at this point (the
 * expander is disabled and the LCD is pure RGB), so this installs the
 * legacy I2C driver, does its writes, and deletes it again -- leaving a
 * clean state for the library's own touch-bus host init that runs next.
 * Without this sequence the GT911 stays in reset and its I2C address never
 * ACKs ("i2c transaction failed" / "GT911 read error").
 */
#define ESP_PANEL_BOARD_TOUCH_PRE_BEGIN_FUNCTION(p) \
    {  \
        constexpr gpio_num_t TP_INT = static_cast<gpio_num_t>(ESP_PANEL_BOARD_TOUCH_INT_IO); \
        i2c_config_t ioext_conf = {}; \
        ioext_conf.mode = I2C_MODE_MASTER; \
        ioext_conf.sda_io_num = GPIO_NUM_8; \
        ioext_conf.scl_io_num = GPIO_NUM_9; \
        ioext_conf.sda_pullup_en = GPIO_PULLUP_ENABLE; \
        ioext_conf.scl_pullup_en = GPIO_PULLUP_ENABLE; \
        ioext_conf.master.clk_speed = 400000; \
        i2c_param_config(I2C_NUM_0, &ioext_conf); \
        bool ioext_installed = (i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK); \
        const uint8_t ioext_mode_all_out[2] = {0x02, 0xFF}; \
        const uint8_t ioext_io1_low[2]      = {0x03, 0xFD}; \
        const uint8_t ioext_io1_high[2]     = {0x03, 0xFF}; \
        i2c_master_write_to_device(I2C_NUM_0, 0x24, ioext_mode_all_out, 2, pdMS_TO_TICKS(50)); \
        vTaskDelay(pdMS_TO_TICKS(10)); \
        gpio_set_direction(TP_INT, GPIO_MODE_OUTPUT); \
        i2c_master_write_to_device(I2C_NUM_0, 0x24, ioext_io1_low, 2, pdMS_TO_TICKS(50)); \
        vTaskDelay(pdMS_TO_TICKS(100)); \
        gpio_set_level(TP_INT, 0); \
        vTaskDelay(pdMS_TO_TICKS(100)); \
        i2c_master_write_to_device(I2C_NUM_0, 0x24, ioext_io1_high, 2, pdMS_TO_TICKS(50)); \
        vTaskDelay(pdMS_TO_TICKS(200)); \
        if (ioext_installed) { \
            i2c_driver_delete(I2C_NUM_0); \
        } \
        gpio_reset_pin(TP_INT); \
        return true;    \
    }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////// File Version ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 0
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0

#endif // ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

// *INDENT-ON*
