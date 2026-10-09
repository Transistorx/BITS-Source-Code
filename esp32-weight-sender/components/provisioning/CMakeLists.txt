idf_component_register(
    SRCS "provisioning.c"
    INCLUDE_DIRS "include"
    REQUIRES app_error nvs_config led_status broker_cfg mqtt_link_core esp_wifi esp_netif esp_event esp_http_server log freertos lwip esp_hw_support
)
