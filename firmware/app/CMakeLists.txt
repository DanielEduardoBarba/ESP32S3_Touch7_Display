# ---------------------------------------------------------------------------
# APP-STAGE ESP-IDF project ("the brains"). See firmware/README.md.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "../common/sdkconfig.defaults;sdkconfig.defaults")

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
