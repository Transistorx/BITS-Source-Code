idf_component_register(
    SRCS "websocket_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES log_util esp_timer
)