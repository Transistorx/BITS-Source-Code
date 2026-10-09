idf_component_register(
    SRCS "mqtt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_link_core nvs_config freertos esp_timer log mqtt log_util
)
