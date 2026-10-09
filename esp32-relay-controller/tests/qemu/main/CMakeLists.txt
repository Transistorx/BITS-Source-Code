idf_component_register(
    SRCS "test_main.c" "test_concurrency.c" "test_mqtt.c" "test_weight_mqtt.c" "test_run_mqtt.c""test_flow_estimator.c" "test_autotune.c" "test_prereq_batch1.c" "test_prereq_fix1.c" "test_single_scale.c"
    INCLUDE_DIRS "."
    # provisioning is required for its PURE AP-name builder
    # (provisioning_build_ap_name). The component also carries the Wi-Fi
    # portal, which is linked but never started here; the builder itself
    # touches no hardware and no state.
    REQUIRES relay_driver weight_receiver websocket_client
             job_queue dispense_controller dual_dispense_controller safety_manager provisioning telemetry_client mqtt_link_core weight_mqtt flow_estimator autotune_core
)
