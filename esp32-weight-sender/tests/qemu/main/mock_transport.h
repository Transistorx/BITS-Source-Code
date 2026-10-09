#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

/*
 * Test-only scale_transport backend.
 *
 * Feeds canned bytes and injected link events to scale_reader so the framing
 * and parser path can be exercised without a UART. Compiled only into the
 * QEMU test project; production firmware never sees these symbols.
 *
 * Its wait_event() blocks for the requested timeout when empty, matching the
 * real backend's "block up to timeout_ms" contract, so the reader task does
 * not spin.
 */

#include "scale_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_XPORT_RX_CAP 4096U
#define MOCK_XPORT_EV_CAP 64U

typedef struct {
    scale_transport_t base;

    uint8_t rx[MOCK_XPORT_RX_CAP];
    size_t rx_len;

    scale_xport_event_t events[MOCK_XPORT_EV_CAP];
    size_t ev_len;

    /* Instrumentation the tests assert on. */
    uint32_t open_calls;
    uint32_t close_calls;
    uint32_t read_calls;
    uint32_t flush_calls;
    uint32_t bytes_served;
    bool opened;
    bool fail_reads;   /* make read() report a transport error */
    uint32_t block_once_ms; /* next wait_event() blocks this long (reader stall) */
} mock_transport_t;

void mock_transport_init(mock_transport_t *t);

/* Append raw wire bytes to the readable stream. */
bool mock_transport_push_bytes(mock_transport_t *t, const uint8_t *data,
                               size_t length);

/* Queue a link event for the reader to consume. */
bool mock_transport_push_event(mock_transport_t *t, scale_xport_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_TRANSPORT_H */
