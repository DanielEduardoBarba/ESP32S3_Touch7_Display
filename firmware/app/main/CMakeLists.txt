idf_component_register(
    SRC_DIRS "." "ui"
    INCLUDE_DIRS "." "ui"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server spiffs fatfs sdmmc driver json app_update
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
