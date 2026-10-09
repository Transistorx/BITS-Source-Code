idf_component_register(
    SRCS "app_main.c" "remote_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_manager websocket_server weight_source
             drivers scale_hal scale_manager scale_protocols interfaces
             board nvs_config provisioning led_status esp_driver_gpio
             mqtt_link mqtt_link_core scale_cmd log_util task_hb
)
