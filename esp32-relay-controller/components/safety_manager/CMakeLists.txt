idf_component_register(
    SRCS "safety_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES relay_driver log esp_common esp_system
)