idf_component_register(
    SRCS "dispense_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES job_queue relay_driver safety_manager weight_receiver log esp_common
)