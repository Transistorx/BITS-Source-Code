idf_component_register(
    SRCS "weight_receiver.c" "weight_slots.c"
    INCLUDE_DIRS "include"
    REQUIRES relay_driver
)
