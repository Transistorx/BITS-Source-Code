#ifndef SCALE_TRANSPORT_H
#define SCALE_TRANSPORT_H

/*
 * Physical-link transport contract for the weighing-scale byte stream.
 *
 * This header is the seam between "how bytes arrive" (RS232 UART today) and
 * "what the bytes mean" (scale_reader framing + scale_protocols parsers).
 * scale_reader must not include driver/uart.h or know which physical layer is
 * underneath; it only consumes this interface.
 *
 * Nothing here encodes RS485 behaviour. DE/RE pins, half-duplex mode,
 * 2-wire vs 4-wire, termination and addressing are all Phase 0-gated
 * hardware facts and deliberately have no representation in this contract.
 * A future RS485 backend slots in behind the same ops without touching the
 * parser or the reader.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport-agnostic link events. These are a stable subset of the physical
 * link's fault signals. The RS232 backend maps uart_event_t onto them; a
 * future backend maps its own. The reader reacts only to this enumeration.
 */
typedef enum {
    SCALE_XPORT_EV_DATA = 0,     /* one or more bytes may be readable */
    SCALE_XPORT_EV_FIFO_OVF,     /* hardware FIFO overflowed */
    SCALE_XPORT_EV_BUFFER_FULL,  /* driver RX ring buffer full */
    SCALE_XPORT_EV_BREAK,        /* break condition on the line */
    SCALE_XPORT_EV_PARITY_ERR,   /* parity error */
    SCALE_XPORT_EV_FRAME_ERR,    /* framing error */
    SCALE_XPORT_EV_PATTERN_DET,  /* pattern detector matched */
} scale_xport_event_type_t;

typedef struct {
    scale_xport_event_type_t type;
} scale_xport_event_t;

/*
 * Event-queue depth the reader treats as "saturated" before it counts a
 * diagnostic. Matches SCALE_UART_EVENT_QUEUE_SIZE used by the RS232 driver
 * install, kept here so the reader can size its expectations without
 * including the UART driver header.
 */
#define SCALE_TRANSPORT_EVENT_QUEUE_SIZE 20

/*
 * Physical layer of the scale link. Both values are board facts resolved by
 * board_pins.h; nothing in the reader or parser branches on them. They exist
 * so startup diagnostics and the link record can say what the wire is.
 *
 * RS485 here means an RS-485 bus behind an auto-direction transceiver
 * (MAX13487E): the ESP32 still drives a plain UART. It does NOT imply
 * UART_MODE_RS485_HALF_DUPLEX or any ESP32-side direction control.
 */
typedef enum {
    SCALE_XPORT_RS232 = 0,
    SCALE_XPORT_RS485_AUTO_DIR = 1
} scale_xport_type_t;

/*
 * Link configuration. Baud/parity/stop remain compile-time (Kconfig) exactly
 * as they are today; this struct carries the per-channel identity of the
 * link and the physical-layer label.
 *
 * There is no de_gpio, no rs485_mode, no device address field: the
 * MAX13487E owns bus direction in hardware, so an ESP32 direction pin would
 * be a wiring error rather than a configuration option.
 */
typedef struct {
    int uart_port;   /* ESP-IDF UART peripheral number */
    int rx_gpio;
    int tx_gpio;
    int baud_rate;
    scale_xport_type_t transport;
    int channel;     /* logical scale channel id (0-based) */
} scale_link_config_t;

typedef struct scale_transport scale_transport_t;

/*
 * Backend operations. All entry points take the transport handle so a backend
 * can keep private state behind `ctx`.
 *
 * Timing units are milliseconds so the contract does not leak FreeRTOS ticks
 * into backends or tests. The RS232 backend converts internally.
 */
typedef struct {
    const char *name;

    /* Bring the link up. Returns ESP_ERR_INVALID_STATE if the configured
     * port is reserved (e.g. the console UART). */
    esp_err_t (*open)(scale_transport_t *self, const scale_link_config_t *cfg);

    /* Block up to timeout_ms for one link event. False on timeout. */
    bool (*wait_event)(scale_transport_t *self, scale_xport_event_t *out,
                       uint32_t timeout_ms);

    /* Read up to max_len bytes. Returns bytes read (>=0) or a negative
     * value on a transport error. 0 means "nothing available". */
    int (*read)(scale_transport_t *self, uint8_t *buf, size_t max_len,
                uint32_t timeout_ms);

    /* Discard any buffered, not-yet-read input. */
    esp_err_t (*flush_input)(scale_transport_t *self);

    /* Bytes currently buffered but not yet read. */
    esp_err_t (*get_buffered_len)(scale_transport_t *self, size_t *out_len);

    /* Events currently queued but not yet consumed. Drives the saturation
     * diagnostic; must not block. */
    size_t (*pending_events)(scale_transport_t *self);

    /* Release backend resources. Safe to call when not open. */
    void (*close)(scale_transport_t *self);
} scale_transport_ops_t;

struct scale_transport {
    const scale_transport_ops_t *ops;
    void *ctx;  /* backend-private */
};

/* Convenience wrappers. Keep call sites readable and null-safe. */
esp_err_t scale_transport_open(scale_transport_t *t, const scale_link_config_t *cfg);
bool scale_transport_wait_event(scale_transport_t *t, scale_xport_event_t *out,
                                uint32_t timeout_ms);
int scale_transport_read(scale_transport_t *t, uint8_t *buf, size_t max_len,
                         uint32_t timeout_ms);
esp_err_t scale_transport_flush_input(scale_transport_t *t);
esp_err_t scale_transport_get_buffered_len(scale_transport_t *t, size_t *out_len);
size_t scale_transport_pending_events(scale_transport_t *t);
void scale_transport_close(scale_transport_t *t);
const char *scale_transport_name(const scale_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* SCALE_TRANSPORT_H */
