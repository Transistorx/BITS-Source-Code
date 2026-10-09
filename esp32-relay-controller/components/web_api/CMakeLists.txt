idf_component_register(
    SRCS "web_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server dispense_controller dual_dispense_controller job_queue safety_manager
             weight_receiver relay_driver websocket_client log esp_timer
)
