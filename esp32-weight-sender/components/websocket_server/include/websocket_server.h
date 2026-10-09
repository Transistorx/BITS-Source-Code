#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* Longest inbound control frame accepted. The relay controller's commands are
 * well under 128 bytes; anything larger is refused unread rather than
 * buffered, so a hostile or corrupt peer cannot drive an allocation here. */
#define WS_CMD_MAX_LEN 256U

/* Called from the httpd task for each complete inbound TEXT frame, with the
 * payload NUL-terminated at len. Must not block: the httpd task serves the
 * socket, including the PONG that keeps the relay controller's liveness probe
 * alive. Registered before the server starts; NULL disables dispatch. */
typedef void (*websocket_server_rx_fn)(const char *payload, size_t len);

void websocket_server_set_rx_handler(websocket_server_rx_fn handler);

esp_err_t websocket_server_start(void);

/* Sends a text frame to the connected client. Returns false when no client is
 * connected; that is a normal condition, not an error. Never blocks. */
bool websocket_server_broadcast(const char *text);
bool websocket_server_has_client(void);

#endif /* WEBSOCKET_SERVER_H */