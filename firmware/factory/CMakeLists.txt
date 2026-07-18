# ---------------------------------------------------------------------------
# FACTORY-STAGE ESP-IDF project. See the repo root README.md for the two-stage
# firmware design (this "factory" splash stage vs. the "app" brains stage).
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# Board variant selection (Waveshare "7" vs "7B", see build.sh's --variant
# flag and the repo root README.md's "Board variants" section). Defaults to "7" if unset.
if(NOT DEFINED ENV{TOUCH_ESP32_VARIANT} OR "$ENV{TOUCH_ESP32_VARIANT}" STREQUAL "")
    set(TOUCH_ESP32_VARIANT "7")
else()
    set(TOUCH_ESP32_VARIANT "$ENV{TOUCH_ESP32_VARIANT}")
endif()

# Board / LVGL / partition table defaults shared with firmware/app, plus any
# factory-stage-only overrides in ./sdkconfig.defaults.
set(SDKCONFIG_DEFAULTS "../common/sdkconfig.defaults;../common/sdkconfig.defaults.${TOUCH_ESP32_VARIANT};sdkconfig.defaults")

# Makes main/conf/esp_panel_board_custom_conf.h discoverable by
# ESP32_Display_Panel's __has_include() check -- only takes effect for the
# "7b" variant (see firmware/common/sdkconfig.defaults.7b); inert otherwise.
include_directories(${CMAKE_CURRENT_LIST_DIR}/main/conf)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(factory)
