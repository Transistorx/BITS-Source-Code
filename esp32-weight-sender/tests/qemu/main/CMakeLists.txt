idf_component_register(
    SRCS "test_main.c" "test_transport_golden.c" "test_board_pinmap.c" "mock_transport.c" "test_responsiveness.c" "test_mqtt_cmd.c" "test_mqtt_stop.c" "test_mqtt_env.c" "test_log_util.c" "test_diag_channels.c" "test_reader_safety.c"
    INCLUDE_DIRS
        "."
        # Frozen RS232 golden vectors, captured before the transport
        # abstraction. Header-only so the expectations cannot drift from a
        # separately compiled copy. See docs/superpowers/baseline/rs232-golden.
        "../../../docs/superpowers/baseline/rs232-golden"
    # provisioning is required for its PURE AP-name builder
    # (provisioning_build_ap_name). The component also carries the Wi-Fi
    # portal, which is linked but never started here; the builder itself
    # touches no hardware and no state.
    #
    # scale_hal is exercised through a mock transport, so the reader's
    # framing and parser dispatch run in the suite without a UART.
    REQUIRES weight_source websocket_server interfaces provisioning
             scale_hal scale_protocols freertos board scale_manager mqtt_link_core scale_cmd esp_timer log_util
)
