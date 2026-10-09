idf_component_register(
    SRCS "uart_driver.c" "scale_transport_rs232.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_driver_uart" "esp_driver_gpio" "interfaces" "freertos" "board"
)
