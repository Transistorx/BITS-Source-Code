idf_component_register(
    SRCS "weight_source.c"
    INCLUDE_DIRS "include"
    REQUIRES scale_manager interfaces esp_common esp_timer log_util
)