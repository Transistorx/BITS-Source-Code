idf_component_register(
    SRCS "scale_reader.c"
    INCLUDE_DIRS "."
    # The reader consumes the scale_transport contract, not a UART. The RS232
    # backend lives in `drivers`, which is the only component that touches
    # esp_driver_uart. Keeping that dependency out of here is what lets the
    # framing/parser path be exercised under QEMU with a mock transport.
    REQUIRES "interfaces" "esp_timer" "log_util"
)
