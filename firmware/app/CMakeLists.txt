# ---------------------------------------------------------------------------
# APP-STAGE ESP-IDF project ("the brains"). See the repo root README.md.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# Board variant selection (Waveshare "7" vs "7B", see build.sh's --variant
# flag and the repo root README.md's "Board variants" section). Defaults to "7" if unset.
if(NOT DEFINED ENV{TOUCH_ESP32_VARIANT} OR "$ENV{TOUCH_ESP32_VARIANT}" STREQUAL "")
    set(TOUCH_ESP32_VARIANT "7")
else()
    set(TOUCH_ESP32_VARIANT "$ENV{TOUCH_ESP32_VARIANT}")
endif()

set(SDKCONFIG_DEFAULTS "../common/sdkconfig.defaults;../common/sdkconfig.defaults.${TOUCH_ESP32_VARIANT};sdkconfig.defaults")

# Makes main/conf/esp_panel_board_custom_conf.h discoverable by
# ESP32_Display_Panel's __has_include() check -- only takes effect for the
# "7b" variant (see firmware/common/sdkconfig.defaults.7b); inert otherwise.
include_directories(${CMAKE_CURRENT_LIST_DIR}/main/conf)

# Stamp the app image with APP_VERSION (from main/app_config.h) as its
# embedded project version (esp_app_desc_t.version). The factory recovery
# menu reads this straight out of each OTA slot's flash to show which
# version is loaded where, and the Update scene / peer sync use it too.
file(READ "${CMAKE_CURRENT_LIST_DIR}/main/app_config.h" _app_config_h)
string(REGEX MATCH "#define APP_VERSION[ \t]+\"([^\"]+)\"" _ "${_app_config_h}")
if(CMAKE_MATCH_1)
    set(PROJECT_VER "${CMAKE_MATCH_1}")
    message(STATUS "App version (from app_config.h): ${PROJECT_VER}")
endif()
# CRITICAL: file(READ) creates no configure dependency on its own -- without
# this line, editing APP_VERSION would NOT re-run CMake and every image would
# keep being stamped with a stale PROJECT_VER (the factory menu then shows
# the wrong/same version for both OTA slots).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/main/app_config.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app)

# Pack the built React UI (../../web/dist) into the `webapp` SPIFFS partition
# so the on-device web server can serve it on port 80.
#
# Run `./build.sh --web` (or `cd web && npm ci && npm run build`) first. If
# web/dist doesn't exist yet (e.g. fresh checkout, Node not installed), fall
# back to a tiny placeholder page so the firmware build doesn't fail.
set(WEB_DIST_DIR "${CMAKE_CURRENT_LIST_DIR}/../../web/dist")
if(NOT EXISTS "${WEB_DIST_DIR}/index.html")
    set(WEB_DIST_DIR "${CMAKE_CURRENT_LIST_DIR}/webapp_placeholder")
    message(WARNING "web/dist not found -- flashing the placeholder webapp image instead. Run `./build.sh --web` to build the real React UI.")
endif()
spiffs_create_partition_image(webapp "${WEB_DIST_DIR}" FLASH_IN_PROJECT)
