idf_component_register(
    SRCS "nvs_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log esp_hw_support
)
