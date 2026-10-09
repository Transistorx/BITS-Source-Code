idf_component_register(
    SRCS "weight_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES weight_receiver mqtt_link_core log
)
