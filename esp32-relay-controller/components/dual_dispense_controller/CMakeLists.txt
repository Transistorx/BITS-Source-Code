idf_component_register(
    SRCS "dual_dispense_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES relay_driver job_queue safety_manager weight_receiver dispense_controller
)
