idf_component_register(
    SRCS "mqtt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_link_core broker_cfg task_hb nvs_config freertos esp_timer log mqtt mbedtls log_util
)
