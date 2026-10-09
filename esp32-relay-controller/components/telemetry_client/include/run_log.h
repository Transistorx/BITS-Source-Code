#ifndef RUN_LOG_H
#define RUN_LOG_H

/*
 * Run lifecycle over MQTT (docs/telemetry/CONTRACT.md 9.2): a bounded RAM ring
 * of run/start, run/samples, run/events and run/complete bodies, published QoS 1
 * (not retained) and kept until the backend's run/ack says it is stored.
 *
 *  - Nothing here blocks: the publish hook is a zero-timeout enqueue and a refused
 *    publish is simply retried by a later pump. No caller is a control task.
 *  - Reconnect: every unacked item is marked unsent and goes out again, oldest first.
 *  - Overflow drops the OLDEST sample batch and counts its samples per run. The batch
 *    shrinks in place to a tombstone {"run_id","batch_seq","dropped":true,"samples":k}
 *    that is published in order, so batch_seq stays contiguous and acks can advance.
 *    Start, events and complete are never dropped while capacity remains; if no slot
 *    can be freed the add is refused and the caller retries (it keeps its own copy).
 *  - The WHOLE ring (every kind) is capped at RUNLOG_TOTAL_BYTES: when an add would pass
 *    it, the oldest sample batches go first; if none are left, new events/samples are
 *    refused (refused_total_cap). start/complete may use RUNLOG_CRITICAL_RESERVE more,
 *    then they are refused too. The ring never grows past TOTAL + RESERVE.
 *  - Slot reserve, staggered (one per channel each): samples/events leave RUNLOG_SLOT_RESERVE (6)
 *    slots free, a start may use down to 4 free, a terminal event (RUNLOG_TERMINAL, sent as
 *    run/events) down to 2, a complete down to 0. So the active run's start, terminal event and
 *    complete always find a slot.
 *  - A tombstone is written in place into the body (every sample/event body is allocated at
 *    least 256 B, so this cannot fail) and the big body is shrunk with realloc, so the byte caps
 *    are the real heap use (a refused shrink keeps the old size accounted).
 *  - Abandonment never depends on wall-clock alone. A timed-out send counts as a try only if
 *    some valid run/ack (any run) arrived after that send; total backend silence just resends
 *    (backoff capped at 80 s) and abandons nothing. After RUNLOG_MAX_RESENDS counted tries
 *    (backoff 10/20/40/80/80/80 s) the run's oldest sample/event becomes a tombstone (counted
 *    resend_abandoned, logged). A tombstone, a start and a complete are NEVER released by a timeout.
 *  - Fairness: the next item sent is the oldest unsent item of the run with the fewest items in
 *    flight, so one stuck or long run cannot starve another within the global window.
 *  - UNKNOWN_RUN: the replies a cycle still owes (items in flight - 1) are absorbed after the pump
 *    re-sent, they do not count as a new cycle.
 *  - An ack only releases items already published at least once.
 *  - An item over RUNLOG_ITEM_MAX fails at enqueue (add_oversize) instead of becoming a
 *    head-of-line item the link could never carry.
 *  - Delivery is at least once (resends after a timeout / reconnect): the backend MUST
 *    stay idempotent on (run_id, batch_seq).
 *  - Telemetry only: no result here is ever read by the control loop.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUNLOG_SLOTS          40U
#define RUNLOG_SAMPLE_BYTES   16384U   /* budget for queued sample payloads */
#ifndef RUNLOG_TOTAL_BYTES
#define RUNLOG_TOTAL_BYTES    24576U   /* cap on ALL queued bodies (start/samples/events/complete) */
#endif
#define RUNLOG_CRITICAL_RESERVE 4096U  /* extra room only start/complete may use */
/* Must not exceed MQTT_LINK_TX_PAYLOAD_MAX (mqtt_link_core.h): the link refuses a bigger
 * body, which would block the ring head forever. Checked by a QEMU test. */
#define RUNLOG_ITEM_MAX       4864U
#define RUNLOG_RESERVE_PER_CLASS 2U    /* start / terminal event / complete: one per channel each */
#define RUNLOG_SLOT_RESERVE   (3U * RUNLOG_RESERVE_PER_CLASS)   /* slots samples/events never take */
#define RUNLOG_WINDOW        2U       /* items in flight (sent, not acked) */
#define RUNLOG_RESEND_MS      10000U   /* unacked item is sent again after this */
#define RUNLOG_MAX_UNKNOWN    3U       /* UNKNOWN_RUN resend cycles before the run is abandoned */
#ifndef RUNLOG_MAX_RESENDS
#define RUNLOG_MAX_RESENDS    6U       /* timed-out sends of a run's oldest item before it is abandoned */
#endif
#define RUNLOG_RUN_ID_MAX     64U

typedef enum {
    RUNLOG_START = 0,
    RUNLOG_SAMPLES,
    RUNLOG_EVENTS,
    RUNLOG_COMPLETE,
    RUNLOG_TERMINAL,   /* a run's terminal event: goes out on run/events, may use a deeper slot reserve */
} runlog_kind_t;

typedef struct {
    bool (*connected)(void);
    /* Zero-timeout enqueue under cas/{device}/{suffix}. False = not queued. */
    bool (*publish)(const char *suffix, const char *json, int qos, bool retain);
} runlog_io_t;

typedef struct {
    uint32_t added, published, resent, acked_items, abandoned_items;
    uint32_t dropped_batches, dropped_samples, add_refused, ack_ignored;
    uint32_t pending, sample_bytes;
    uint32_t total_bytes, add_oversize, refused_total_cap;
    uint32_t tombstones, rejected_runs;
    uint32_t resend_abandoned;   /* samples/events tombstoned after RUNLOG_MAX_RESENDS counted tries */
    uint32_t superseded_runs, superseded_unclosed;   /* un-closed run replaced by a new job; unclosed = its complete was refused */
} runlog_stats_t;

/* A pending run was closed as UNKNOWN (status FAILED, error RUN_SUPERSEDED) before its slot was reused. */
void runlog_note_superseded(bool closed);
/* Test seam: make the tombstone shrink-realloc "fail" (the tombstone must still be valid). */
void runlog_test_fail_tomb_shrink(bool on);

/* Test seam: how many times the ring lock was taken (read without the lock). */
uint32_t runlog_lock_takes(void);

/* Clears the ring (frees every body) and installs the hooks. Returns false if the
 * ring could not be allocated (nothing is installed; runlog_add then refuses). */
bool runlog_init(const runlog_io_t *io);

/* Copies `json` into the ring. batch_seq: start = 0, samples/events share the
 * run's counter, complete = end_seq + 1. n_samples is only for kind SAMPLES. */
bool runlog_add(runlog_kind_t kind, const char *run_id, uint32_t batch_seq,
                uint32_t n_samples, const char *json, size_t len);

/* Publishes unsent items (oldest per run, fair across runs) while fewer than RUNLOG_WINDOW are in
 * flight. Returns the number published. Safe from any non-ISR task. */
size_t runlog_pump(uint32_t now_ms);

/* run/ack body: {"run_id","acked_batch_seq","state"}. Trims on a stored ack. */
void runlog_on_ack(const char *json, size_t len);

uint32_t runlog_dropped_samples(const char *run_id);   /* overflow drops for this run */
uint32_t runlog_pending(void);
uint32_t runlog_pending_for_run(const char *run_id);
void runlog_stats(runlog_stats_t *out);

#endif
