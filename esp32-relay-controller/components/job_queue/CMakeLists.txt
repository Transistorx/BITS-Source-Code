idf_component_register(
    SRCS "job_queue.c"
    INCLUDE_DIRS "include"
    REQUIRES log freertos esp_common
)