idf_component_register(
    SRCS "telemetry_client.c" "run_log.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client nvs_config weight_receiver websocket_client dual_dispense_controller job_queue mqtt_link_core safety_manager esp_timer
)
