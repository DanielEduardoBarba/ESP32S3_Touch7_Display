idf_component_register(
    SRCS "main.cpp" "lvgl_v8_port.cpp"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
