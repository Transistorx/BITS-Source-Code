idf_component_register(
    SRCS "relay_driver.c" "relay_expander.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
)
