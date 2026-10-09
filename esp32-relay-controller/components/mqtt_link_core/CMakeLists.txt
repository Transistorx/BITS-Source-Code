idf_component_register(
    SRCS "mqtt_link_core.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_hw_support
)
