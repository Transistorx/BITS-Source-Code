#ifndef SCALE_READER_H
#define SCALE_READER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol_interface.h"
#include "scale_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The reader consumes a byte stream plus link events through
 * scale_transport. It knows nothing about RS232, RS485, UART peripherals or
 * GPIOs: those live behind the transport backend. Framing, parser dispatch,
 * the two-frame ONLINE rule and every diagnostic counter are unchanged.
 */
typedef struct {
    uint8_t channel_id;
    scale_transport_t *transport;
    const scale_protocol_t** protocols;
    size_t num_protocols;
    QueueHandle_t event_queue;

    /* ---- Safety policy. A zeroed value is the PRODUCTION setting. ----------
     * allow_auto_detect: false (default) = only protocols[0] is ever consulted
     *   and num_protocols must be 1. true tries every protocol until the link
     *   is confirmed, then LOCKS to the confirmed one while ONLINE. Honoured
     *   only when CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY is built in; the
     *   production configuration rejects it at scale_reader_start().
     * allow_generic_fallback: the loose "number + unit" ASCII parser. Same
     *   restriction; off in production because it parses frame tails.
     * strict_pad / pad_value: reject a CI-150A frame whose byte 17 differs from
     *   pad_value. The real value is NOT documented: set it from the bench
     *   capture (RAW log line "pad=0x..") before enabling.
     * stall_gap_ms: a reader-loop gap above this flushes the RX ring and
     *   resyncs to the next terminator (0 = CONFIG_SCALE_READER_STALL_GAP_MS). */
    bool allow_auto_detect;
    bool allow_generic_fallback;
    bool strict_pad;
    uint8_t pad_value;
    uint32_t stall_gap_ms;
} scale_reader_config_t;

/* Why the reader changed its link state. Written once per transition. */
typedef enum {
    SCALE_LINK_REASON_NONE = 0,
    SCALE_LINK_REASON_CONFIRMED,          /* 2 consecutive valid frames within 3 s */
    SCALE_LINK_REASON_TIMEOUT_NO_BYTES,   /* >=3 s without a valid frame, no RX bytes at all */
    SCALE_LINK_REASON_TIMEOUT_REJECTED,   /* >=3 s without a valid frame, bytes arrived but were rejected/incomplete */
    SCALE_LINK_REASON_TIMEOUT_OVERLOAD    /* >=3 s without a valid frame, the scale reported OL (overload) */
} scale_link_reason_t;

#define SCALE_RAW_TAIL_SIZE 40U
#define SCALE_OL_RAW_SIZE 24U

typedef struct {
    uint32_t bytes_received;
    uint32_t frames_received;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t framing_errors;
    uint32_t parity_errors;
    uint32_t break_events;
    uint32_t pattern_detections;
    uint32_t fifo_overflows;
    uint32_t driver_buffer_overflows;
    uint32_t parser_overflows;
    uint32_t dropped_bytes;
    uint32_t application_queue_drops;
    uint32_t uart_event_queue_saturations;
    uint32_t uart_read_errors;
    uint32_t application_queue_high_water;
    uint32_t parser_calls;
    uint32_t parser_min_us;
    uint32_t parser_max_us;
    uint64_t parser_total_us;
    uint32_t stack_high_water_bytes;
    /* Any UART byte activity; does not establish a scale connection. */
    uint64_t last_rx_timestamp_ms;
    /* Most recent parser-accepted complete frame, including before ONLINE confirmation. */
    uint64_t last_valid_frame_timestamp_ms;

    /* Rejected complete frames by reason. Their sum equals invalid_frames.
     * Over-long lines with no terminator are counted in parser_overflows
     * (the terminator-missing counter). The CI-150A format has no checksum:
     * rej_field covers the weight and unit fields. */
    uint32_t rej_length;
    uint32_t rej_header;   /* separators, GS/NT field, device ID */
    uint32_t rej_status;
    uint32_t rej_field;
    uint32_t rej_other;
    /* Further rejected-frame categories; they are part of invalid_frames too:
     * rej_length+hdr+status+field+other+format+pad == invalid_frames.
     *   rej_format: decimal-point/unit format differs from the established one
     *   rej_pad:    strict_pad enabled and byte 17 != the configured value */
    uint32_t rej_format;
    uint32_t rej_pad;
    /* OL (overload) frames. They are neither valid nor invalid: no weight is
     * emitted, the link is not refreshed, and the OFFLINE reason becomes
     * "overload". */
    uint32_t overload_frames;
    /* Reader-loop stall recoveries (RX ring flushed, resync to terminator) and
     * the number of times the assembler was told to discard until the next
     * terminator after any flush/overflow/stall recovery. */
    uint32_t reader_stalls;
    uint32_t last_stall_gap_ms;
    uint32_t resync_events;
    /* LF bytes kept as data inside a CI-150A body (device ID, lamp octet, pad). */
    uint32_t embedded_lf;

    /* Accepted frames by status word. A scale in "stable only" output mode
     * (CI-150A COMM CMD2.3) only ever produces ST frames and goes silent while
     * the weight moves; stream mode (CMD2.2) produces both. */
    uint32_t valid_stable;
    uint32_t valid_unstable;

    /* Longest gap between two passes of the reader task loop (ms). A value
     * near or above 3000 would mean the READER stalled (RX ring overflows
     * and a false link drop), as opposed to the scale going silent. */
    uint32_t loop_gap_max_ms;

    /* RAW CAPTURE for the bench: the last bytes received (not necessarily a
     * whole frame), the first frame containing "OL", and the pad / lamp octet
     * of the last accepted frame (the real CI-150A values are undocumented). */
    uint8_t  raw_tail[SCALE_RAW_TAIL_SIZE];
    uint8_t  raw_tail_len;
    bool     ol_captured;
    uint8_t  ol_raw[SCALE_OL_RAW_SIZE];
    uint8_t  ol_raw_len;
    bool     have_pad_lamp;
    uint8_t  last_pad;
    uint8_t  last_lamp;

    /* Most recent accepted weight on THIS channel (genuine CAS data only). */
    bool     has_last_weight;
    int32_t  last_weight_g;
    bool     last_stable;
    uint64_t last_weight_ms;

    /* Reader link state (2 valid frames -> online, 3 s without one -> offline)
     * and the last transition, with the counters at that instant. */
    bool     link_online;
    uint32_t link_transitions;
    scale_link_reason_t link_reason;
    uint64_t link_transition_ms;
    uint32_t link_trans_bytes;
    uint32_t link_trans_valid;
    uint32_t link_trans_rejected;   /* invalid_frames + parser_overflows */
} scale_reader_diagnostics_t;

/* Short text for a link transition reason (diagnostics line). */
const char *scale_link_reason_name(scale_link_reason_t reason);

/* Start a scale reader task for a specific channel */
esp_err_t scale_reader_start(const scale_reader_config_t* config);

/* Copy a stable snapshot of the per-channel UART/parser diagnostics. */
bool scale_reader_get_diagnostics(uint8_t channel_id,
                                  scale_reader_diagnostics_t *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif // SCALE_READER_H
