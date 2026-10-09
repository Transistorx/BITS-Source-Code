#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* One weight message is well under 256 bytes; 512 leaves room and bounds the
 * worst case from a corrupt or hostile sender. */
#define WS_RX_BUFFER_SIZE 512U

/* Resolves the server (configured static IP first; mDNS only as an optional
 * diagnostic when no IP is set) and starts the client. Returns
 * ESP_ERR_INVALID_STATE if a client is already live. The client then owns
 * reconnection: bounded, non-blocking retries at its reconnect_timeout_ms —
 * the device never reboots because the peer is temporarily unavailable. */
esp_err_t websocket_client_start(void);

bool websocket_client_is_connected(void);

/* false = the WS stays connected (valve-state upstream still works) but neither
 * feeds weight_receiver nor reports link loss. Used when weight_transport=mqtt. */
void websocket_client_set_feed_enabled(bool enabled);

/* Sends one text frame UPSTREAM to the weight sender over the same link that
 * carries the weights. Used to publish the valve state, which is what lets the
 * sender's dynamic virtual scale respond to real relay commands.
 *
 * BLOCKS up to `timeout_ms` on the socket, so callers must not be the control
 * loop: app_main drains a small queue onto this from its own task. Returns
 * false when no client is connected or the send failed. */
bool websocket_client_send_text(const char *text, uint32_t timeout_ms);

/* The ws://... URI the live client is bound to ("" before the first successful
 * start). Used by the connect supervisor's rate-limited progress log so a
 * wrong target is instantly obvious in the field. */
const char *websocket_client_target(void);

/* Reassembly over the component's fragmented DATA events. Returns true once a
 * complete payload is buffered, or once a payload has overflowed the buffer
 * (in which case the buffered text is truncated and will not parse). */
bool ws_rx_accumulate(const char *fragment, size_t len,
                      size_t payload_len, size_t payload_offset);
const char *ws_rx_buffer(void);
size_t ws_rx_length(void);
void ws_rx_reset(void);

#endif /* WEBSOCKET_CLIENT_H */