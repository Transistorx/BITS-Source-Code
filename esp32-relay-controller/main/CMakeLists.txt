idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES relay_driver wifi_manager websocket_client weight_receiver
             nvs_config provisioning led_status esp_driver_gpio
             job_queue dispense_controller dual_dispense_controller safety_manager web_api telemetry_client mqtt_link mqtt_link_core weight_mqtt flow_estimator autotune_core
)
