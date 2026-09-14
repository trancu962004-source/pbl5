idf_component_register(
    SRCS
        "app_main.c"
        "ble_node.c"
        "sensor_node.c"
    INCLUDE_DIRS "."
    REQUIRES bt
)