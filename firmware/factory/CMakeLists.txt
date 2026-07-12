# ---------------------------------------------------------------------------
# FACTORY-STAGE ESP-IDF project. See firmware/README.md for the two-stage
# firmware design (this "factory" splash stage vs. the "app" brains stage).
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# Board / LVGL / partition table defaults shared with firmware/app, plus any
# factory-stage-only overrides in ./sdkconfig.defaults.
set(SDKCONFIG_DEFAULTS "../common/sdkconfig.defaults;sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(factory)
