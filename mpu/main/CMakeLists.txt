idf_component_register(
    SRCS "mpu.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
)
target_link_libraries(${COMPONENT_LIB} INTERFACE m)