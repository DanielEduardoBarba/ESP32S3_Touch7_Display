idf_component_register(
    SRCS "main.cpp" "lvgl_v8_port.cpp"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# Production build (`./build.sh --prod`): compile out development-only UI
# (the FPS/CPU/RAM/PSRAM overlay). Default is dev (APP_DEV_MODE=1 in
# app_config.h); build.sh's stage marker forces a clean when this changes.
if(DEFINED ENV{TOUCH_ESP32_PROD} AND "$ENV{TOUCH_ESP32_PROD}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_DEV_MODE=0)
endif()
