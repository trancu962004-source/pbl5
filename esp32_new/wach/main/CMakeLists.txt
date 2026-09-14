idf_component_register(
    SRCS "wach.c" "max30100.c" 
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt driver
)