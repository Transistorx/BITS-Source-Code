/* Run lifecycle over MQTT (CONTRACT 9.2) and the profile handshake (9.3).
 * Pure logic: the broker is a mock publish hook, the backend is a few lines in
 * this file that ack what it is given. Nothing here touches a relay. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dual_dispense_controller.h"
#include "job_queue.h"
#include "mqtt_link_core.h"
#include "relay_driver.h"
#include "run_log.h"
#include "safety_manager.h"
#include "telemetry_client.h"

extern void audit_check(bool condition, const char *name);
#define CK(cond, name) audit_check((cond), (name))

/* ---- run ring mock broker -------------------------------------------------- */
typedef struct { char suffix[16]; int qos; bool retain; uint32_t batch_seq; size_t len; char body[400]; } rec_t;
typedef struct { char suffix[24]; char body[400]; } tpub_t;
/* mock backend run: stores (run_id,batch_seq) once, in order, acks the cumulative watermark */
typedef struct { char id[12]; long poison; uint32_t next, applied, dups, tombs, complete_ms; bool started, closed; } be_t;
/* The QEMU image has almost no static DRAM left: the working buffers live on the heap. */
static struct run_ctx {
    rec_t rec[12];
    tpub_t tp[6];
    be_t be[2];
    char body[2200];
    char js[1200];
    mqtt_rx_item_t item;
    mqtt_arx_item_t aitem;
} *g;
#define s_rec (g->rec)
#define s_tp (g->tp)
#define s_body (g->body)
#define s_js (g->js)
#define s_item (g->item)
static int s_rec_n;
static bool s_up;
static bool s_accept;

static bool rl_connected(void) { return s_up; }
static bool rl_publish(const char *suffix, const char *json, int qos, bool retain)
{
    if (!s_accept || s_rec_n >= 12) return false;
    rec_t *r = &s_rec[s_rec_n++];
    snprintf(r->suffix, sizeof(r->suffix), "%s", suffix);
    r->qos = qos;
    r->retain = retain;
    r->len = strlen(json);
    snprintf(r->body, sizeof(r->body), "%s", json);
    const char *p = strstr(json, "\"batch_seq\":");
    r->batch_seq = p ? (uint32_t)strtoul(p + 12, NULL, 10) : 0U;
    return true;
}
static const runlog_io_t s_io = { .connected = rl_connected, .publish = rl_publish };

static void rl_reset(void)
{
    runlog_init(&s_io);
    s_rec_n = 0;
    memset(s_rec, 0, sizeof(s_rec));
    s_up = true;
    s_accept = true;
}

static bool add(runlog_kind_t k, const char *run, uint32_t seq, uint32_t ns, size_t pad)
{
    int n = snprintf(s_body, sizeof(s_body), "{\"run_id\":\"%s\",\"batch_seq\":%lu,\"x\":\"", run, (unsigned long)seq);
    for (size_t i = 0; i < pad && n < (int)sizeof(s_body) - 4; i++) s_body[n++] = 'a';
    s_body[n++] = '"';
    s_body[n++] = '}';
    s_body[n] = '\0';
    return runlog_add(k, run, seq, ns, s_body, (size_t)n);
}
static void ack(const char *run, const char *state, long seq)
{
    char b[200];
    if (seq >= 0) snprintf(b, sizeof(b), "{\"run_id\":\"%s\",\"acked_batch_seq\":%ld,\"state\":\"%s\"}", run, seq, state);
    else snprintf(b, sizeof(b), "{\"run_id\":\"%s\",\"state\":\"%s\"}", run, state);
    runlog_on_ack(b, strlen(b));
}

static void test_ring(void)
{
    const char *R = "dev-ch1-j1-100-abcd";
    rl_reset();
    CK(add(RUNLOG_START, R, 0, 0, 20) && add(RUNLOG_EVENTS, R, 1, 0, 20) && add(RUNLOG_SAMPLES, R, 2, 5, 100) &&
       add(RUNLOG_SAMPLES, R, 3, 5, 100) && runlog_pending() == 4U, "runlog_items_retained");
    s_up = false;
    CK(runlog_pump(1000U) == 0U && s_rec_n == 0, "runlog_no_publish_while_disconnected");
    s_up = true;
    CK(runlog_pump(1000U) == RUNLOG_WINDOW && s_rec_n == (int)RUNLOG_WINDOW, "runlog_window_limits_inflight");
    CK(!strcmp(s_rec[0].suffix, "run/start") && !strcmp(s_rec[1].suffix, "run/events") &&
       s_rec[0].qos == 1 && !s_rec[0].retain && s_rec[1].qos == 1 && !s_rec[1].retain,
       "runlog_oldest_first_qos1_not_retained");
    CK(runlog_pump(1001U) == 0U, "runlog_window_full_until_acked");

    /* ack trimming */
    ack(R, "START_OK", 0);
    CK(runlog_pending() == 3U, "runlog_start_ok_trims_start_only");
    CK(runlog_pump(1002U) == 1U && s_rec[2].batch_seq == 2U, "runlog_acked_frees_window_next_batch_goes");
    ack(R, "BATCH_OK", 2);
    CK(runlog_pending() == 1U && runlog_pending_for_run(R) == 1U, "runlog_batch_ok_trims_up_to_acked_seq");
    ack("other-run", "BATCH_OK", 99);
    CK(runlog_pending() == 1U, "runlog_ack_for_other_run_ignored");
    ack(R, "BATCH_OK", -1);
    runlog_on_ack("not json", 8U);
    runlog_on_ack(NULL, 0U);
    CK(runlog_pending() == 1U, "runlog_malformed_or_incomplete_ack_ignored");

    /* reconnect: everything unacked goes again, oldest first; acked items never */
    rl_reset();
    CK(add(RUNLOG_START, R, 0, 0, 20) && add(RUNLOG_SAMPLES, R, 1, 5, 100) && add(RUNLOG_SAMPLES, R, 2, 5, 100) &&
       add(RUNLOG_SAMPLES, R, 3, 5, 100), "runlog_resend_setup");
    (void)runlog_pump(0U);
    ack(R, "START_OK", 0);
    (void)runlog_pump(10U);
    ack(R, "BATCH_OK", 1);              /* 0 and 1 stored; 2 in flight (unacked) */
    CK(s_rec_n == 3, "runlog_resend_before_state");
    s_up = false;
    (void)runlog_pump(20U);             /* link drops */
    s_up = true;
    int before = s_rec_n;
    CK(runlog_pump(30U) == RUNLOG_WINDOW && s_rec[before].batch_seq == 2U && s_rec[before + 1].batch_seq == 3U,
       "runlog_reconnect_resends_from_last_acked_batch");
    CK(runlog_pending() == 2U, "runlog_reconnect_keeps_unacked_only");
    runlog_stats_t st;
    runlog_stats(&st);
    CK(st.resent >= 1U, "runlog_resend_counted");

    /* an unacked item is resent after the timeout even without a reconnect */
    before = s_rec_n;
    CK(runlog_pump(30U + RUNLOG_RESEND_MS - 1U) == 0U && runlog_pump(30U + RUNLOG_RESEND_MS) == RUNLOG_WINDOW &&
       s_rec[before].batch_seq == 2U, "runlog_unacked_resent_after_timeout");

    /* closing acks drop the whole run */
    ack(R, "COMPLETE", 3);
    CK(runlog_pending() == 0U, "runlog_complete_ack_releases_run");
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)add(RUNLOG_COMPLETE, R, 9, 0, 20);
    ack(R, "REJECTED", -1);
    CK(runlog_pending() == 0U, "runlog_rejected_ack_releases_run");
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)runlog_pump(0U);
    ack(R, "UNKNOWN_RUN", -1);
    before = s_rec_n;
    CK(runlog_pump(1U) == 1U && s_rec[before].batch_seq == 0U && !strcmp(s_rec[before].suffix, "run/start"),
       "runlog_unknown_run_resends_from_start");
    for (int i = 0; i < (int)RUNLOG_MAX_UNKNOWN; i++) { (void)runlog_pump(100U + (uint32_t)i); ack(R, "UNKNOWN_RUN", -1); }
    CK(runlog_pending() == 0U, "runlog_unknown_run_loop_abandoned");
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    ack(R, "INTERRUPTED", -1);
    CK(runlog_pending() == 0U, "runlog_interrupted_ack_releases_run");
}

static void test_overflow(void)
{
    const char *R = "dev-ch1-j2-200-beef";
    rl_reset();
    CK(add(RUNLOG_START, R, 0, 0, 20) && add(RUNLOG_EVENTS, R, 1, 0, 20), "runlog_overflow_setup");
    uint32_t sent_batches = 0U;
    for (uint32_t i = 0U; i < 30U; i++) {      /* 30 x ~2 KB >> 16 KB budget */
        if (add(RUNLOG_SAMPLES, R, 2U + i, 5U, 1950)) sent_batches++;
    }
    runlog_stats_t st;
    runlog_stats(&st);
    CK(sent_batches == 30U && st.dropped_batches > 0U && st.sample_bytes <= RUNLOG_SAMPLE_BYTES,
       "runlog_overflow_stays_inside_sample_budget");
    CK(st.dropped_samples == st.dropped_batches * 5U && runlog_dropped_samples(R) == st.dropped_samples,
       "runlog_overflow_counts_dropped_samples_per_run");
    CK(add(RUNLOG_COMPLETE, R, 99, 0, 20), "runlog_complete_added_after_overflow");
    /* drain through a backend that acks in order: start, events and complete all arrive */
    bool saw_start = false, saw_event = false, saw_complete = false, oldest_gone = true;
    for (uint32_t t = 0U; t < 200U; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        for (int i = 0; i < s_rec_n; i++) {
            if (!strcmp(s_rec[i].suffix, "run/start")) { saw_start = true; ack(R, "START_OK", 0); }
            else if (!strcmp(s_rec[i].suffix, "run/events")) { saw_event = true; ack(R, "BATCH_OK", s_rec[i].batch_seq); }
            else if (!strcmp(s_rec[i].suffix, "run/samples")) {
                if (s_rec[i].batch_seq == 2U && !strstr(s_rec[i].body, "\"dropped\":true")) oldest_gone = false;   /* OLDEST batch became a tombstone */
                ack(R, "BATCH_OK", s_rec[i].batch_seq);
            } else { saw_complete = true; }
        }
    }
    CK(saw_start && saw_event && saw_complete, "runlog_start_events_complete_survive_overflow");
    CK(oldest_gone, "runlog_overflow_drops_oldest_samples_first");

    /* start/events/complete are never dropped: when the slots are full of them the add is refused */
    rl_reset();
    int ok = 0;
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) if (add(RUNLOG_EVENTS, R, i + 1U, 0, 10)) ok++;
    CK(ok == (int)(RUNLOG_SLOTS - RUNLOG_SLOT_RESERVE), "runlog_events_leave_reserve_slots_free");
    /* N4: staggered reserve, 2 slots per class: start (down to 4 free), terminal event (2), complete (0) */
    CK(!add(RUNLOG_EVENTS, R, 100, 0, 10) && !add(RUNLOG_SAMPLES, R, 102, 5, 10), "reserve_generic_refused_at_6_free");
    CK(add(RUNLOG_START, "o1", 0, 0, 10) && add(RUNLOG_START, "o2", 0, 0, 10) && !add(RUNLOG_START, "o3", 0, 0, 10),
       "reserve_start_has_its_own_two_slots");
    CK(add(RUNLOG_TERMINAL, R, 101, 0, 10) && add(RUNLOG_TERMINAL, "o1", 1, 0, 10) && !add(RUNLOG_TERMINAL, "o2", 1, 0, 10),
       "reserve_terminal_event_has_its_own_two_slots");
    CK(add(RUNLOG_COMPLETE, R, 102, 0, 10) && add(RUNLOG_COMPLETE, "o1", 2, 0, 10) && !add(RUNLOG_COMPLETE, R, 103, 0, 10) &&
       runlog_pending() == RUNLOG_SLOTS, "reserve_complete_has_its_own_two_slots_never_drops");
    runlog_stats(&st);
    CK(st.dropped_batches == 0U && st.add_refused == 11U, "runlog_refusals_counted_nothing_dropped");
    /* samples never take the last slots (tombstones keep theirs): complete stays reachable */
    rl_reset();
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) (void)add(RUNLOG_SAMPLES, R, i + 1U, 5, 10);
    CK(runlog_pending() == RUNLOG_SLOTS - RUNLOG_SLOT_RESERVE && add(RUNLOG_COMPLETE, R, 500, 0, 10),
       "runlog_slot_reserve_keeps_complete_reachable");
}

static void ack_raw(const char *b) { runlog_on_ack(b, strlen(b)); }

/* M1: the exact acks the backend sends with acked_batch_seq:-1. */
static void test_ack_minus_one(void)
{
    const char *R = "dev-ch1-j8-800-0008";
    runlog_stats_t st;
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)add(RUNLOG_SAMPLES, R, 1, 5, 20);
    (void)add(RUNLOG_COMPLETE, R, 2, 0, 20);
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-1,\"state\":\"REJECTED\"}");
    runlog_stats(&st);
    CK(runlog_pending() == 0U && st.rejected_runs == 1U && st.abandoned_items == 3U, "ack_rejected_minus1_releases_and_counts");
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)runlog_pump(0U);
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-1,\"state\":\"UNKNOWN_RUN\"}");
    int before = s_rec_n;
    CK(runlog_pump(1U) == 1U && !strcmp(s_rec[before].suffix, "run/start"), "ack_unknown_run_minus1_resends_start");
    for (int i = 0; i < (int)RUNLOG_MAX_UNKNOWN; i++) {
        (void)runlog_pump(100U + (uint32_t)i);
        ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-1,\"state\":\"UNKNOWN_RUN\"}");
    }
    CK(runlog_pending() == 0U, "ack_unknown_run_minus1_abandoned_after_3");
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-1,\"state\":\"BATCH_OK\"}");
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-2,\"state\":\"REJECTED\"}");
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-01,\"state\":\"REJECTED\"}");
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-1.5,\"state\":\"REJECTED\"}");
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":99999999999,\"state\":\"BATCH_OK\"}");
    ack_raw("{\"run_id\":\"dev-ch1-j8-800-0008\",\"acked_batch_seq\":-,\"state\":\"REJECTED\"}");
    CK(runlog_pending() == 1U, "ack_hostile_negative_or_overflow_ignored");
    rl_reset();
}

/* M2: overflow leaves tombstones, so a cumulative-ack backend can close the run. */
static void test_tombstones(void)
{
    const char *R = "dev-ch1-j9-900-0009";
    runlog_stats_t st;
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)add(RUNLOG_EVENTS, R, 1, 0, 20);
    for (uint32_t i = 0U; i < 30U; i++) (void)add(RUNLOG_SAMPLES, R, 2U + i, 5U, 1950);
    CK(add(RUNLOG_COMPLETE, R, 32, 0, 20), "tomb_complete_added_after_overflow");
    runlog_stats(&st);
    CK(st.dropped_batches > 0U && st.tombstones == st.dropped_batches && st.sample_bytes <= RUNLOG_SAMPLE_BYTES &&
       st.total_bytes <= RUNLOG_TOTAL_BYTES + RUNLOG_CRITICAL_RESERVE, "tomb_counted_and_inside_caps");
    /* backend: acks are cumulative, an out-of-order item is not acked (gap) */
    uint32_t next = 0U;
    int tombs_seen = 0;
    bool tomb_small = true, closed = false;
    for (uint32_t t = 0U; t < 300U && !closed; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        for (int i = 0; i < s_rec_n; i++) {
            if (s_rec[i].batch_seq != next) continue;
            if (!strcmp(s_rec[i].suffix, "run/complete")) { ack(R, "COMPLETE_PARTIAL", (long)next); closed = true; break; }
            if (strstr(s_rec[i].body, "\"dropped\":true")) {
                tombs_seen++;
                if (s_rec[i].len > 200U || !strstr(s_rec[i].body, "\"samples\":5")) tomb_small = false;
            }
            ack(R, next == 0U ? "START_OK" : "BATCH_OK", (long)next);
            next++;
        }
    }
    CK(closed && runlog_pending() == 0U, "tomb_overflow_then_complete_closes_with_cumulative_ack");
    CK(tombs_seen == (int)st.tombstones && tomb_small, "tomb_published_in_order_and_small");

    /* never grows unbounded, complete stays reachable, with no link at all */
    rl_reset();
    s_up = false;
    (void)add(RUNLOG_START, R, 0, 0, 20);
    bool within = true;
    for (uint32_t i = 0U; i < 500U; i++) {
        (void)add(RUNLOG_SAMPLES, R, 1U + i, 5U, 1950);
        runlog_stats(&st);
        if (st.total_bytes > RUNLOG_TOTAL_BYTES || runlog_pending() > RUNLOG_SLOTS - RUNLOG_SLOT_RESERVE) within = false;
    }
    CK(within && add(RUNLOG_COMPLETE, R, 9999, 0, 20), "tomb_ring_bounded_and_complete_reachable");
    rl_reset();
}

/* Third review round: real heap cap, -1 on START_OK, abandonment, ack clamp, UNKNOWN_RUN cycles. */
static void test_review3(void)
{
    const char *R = "dev-ch1-jA-1000-000a", *R2 = "dev-ch2-jB-1100-000b";
    runlog_stats_t st;
    (void)R2;

    /* 1: tombstones return the big body to the heap. 38 slots, long offline flood, REAL heap. */
    rl_reset();
    s_up = false;
    size_t base = heap_caps_get_free_size(MALLOC_CAP_8BIT), worst = 0U;
    (void)add(RUNLOG_START, R, 0, 0, 20);
    for (uint32_t i = 0U; i < 500U; i++) {
        (void)add(RUNLOG_SAMPLES, R, 1U + i, 5U, 1950);
        size_t fr = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (base > fr && base - fr > worst) worst = base - fr;
    }
    runlog_stats(&st);
    CK(runlog_pending() == RUNLOG_SLOTS - RUNLOG_SLOT_RESERVE, "heap_cap_ring_holds_slots_minus_reserve");
    CK(st.tombstones >= 25U, "heap_cap_most_slots_tombstoned");   /* old code kept ~2 KB per tombstone: ~75 KB */
    CK(worst > 0U && worst <= RUNLOG_TOTAL_BYTES + RUNLOG_CRITICAL_RESERVE + RUNLOG_SLOTS * 40U,
       "heap_cap_real_with_38_tombstoned_slots");

    /* 2: -1 never trims; only an absent field means 0 for START_OK */
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)runlog_pump(0U);
    ack_raw("{\"run_id\":\"dev-ch1-jA-1000-000a\",\"acked_batch_seq\":-1,\"state\":\"START_OK\"}");
    CK(runlog_pending() == 1U, "start_ok_minus1_does_not_trim");
    ack(R, "START_OK", -1);   /* field absent */
    CK(runlog_pending() == 0U, "start_ok_absent_field_trims_start");

    /* 5: a huge in-range ack never releases items that were not published */
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)add(RUNLOG_EVENTS, R, 1, 0, 20);
    (void)add(RUNLOG_EVENTS, R, 2, 0, 20);
    (void)runlog_pump(0U);   /* window of 2: start and seq 1 are out, seq 2 is not */
    ack_raw("{\"run_id\":\"dev-ch1-jA-1000-000a\",\"acked_batch_seq\":4294967295,\"state\":\"BATCH_OK\"}");
    CK(runlog_pending() == 1U, "huge_ack_does_not_release_unpublished_items");

    /* 3: (N1) a start is never released by a timeout, with a silent backend or an alive one */
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    for (uint32_t k = 0U; k < 20U; k++) (void)runlog_pump(k * 100000U);
    runlog_stats(&st);
    CK(runlog_pending() == 1U && st.resend_abandoned == 0U && st.abandoned_items == 0U, "silent_start_never_released");
    for (uint32_t k = 20U; k < 40U; k++) { (void)runlog_pump(k * 100000U); ack("elsewhere", "BATCH_OK", 1); }
    runlog_stats(&st);
    CK(runlog_pending() == 1U && st.resend_abandoned == 0U && st.abandoned_items == 0U, "unacked_start_never_released_backend_alive");
    /* 7: UNKNOWN_RUN counts per resend cycle, not per ack */
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 20);
    (void)add(RUNLOG_EVENTS, R, 1, 0, 20);
    (void)runlog_pump(0U);
    for (int i = 0; i < 5; i++) ack(R, "UNKNOWN_RUN", -1);   /* replies of one cycle */
    CK(runlog_pending() == 2U, "unknown_run_burst_in_one_cycle_counts_once");
    for (uint32_t c = 1U; c <= RUNLOG_MAX_UNKNOWN; c++) {
        (void)runlog_pump(c);
        ack(R, "UNKNOWN_RUN", -1);
        ack(R, "UNKNOWN_RUN", -1);
        if (c < RUNLOG_MAX_UNKNOWN - 1U) continue;
        CK(runlog_pending() == (c == RUNLOG_MAX_UNKNOWN ? 0U : 2U), "unknown_run_abandoned_after_cycles");
    }
    rl_reset();
}

static void test_never_block(void)
{
    const char *R = "dev-ch2-j3-300-0001";
    rl_reset();
    s_accept = false;                          /* broker queue refuses everything */
    int64_t t0 = esp_timer_get_time();
    uint32_t pumped = 0U;
    for (uint32_t i = 0U; i < 400U; i++) {
        (void)add(RUNLOG_SAMPLES, R, i + 1U, 5, 600);
        pumped += (uint32_t)runlog_pump(i);
    }
    int64_t dt = esp_timer_get_time() - t0;
    CK(pumped == 0U && dt < 2000000, "runlog_refused_publish_never_blocks_producer_or_pump");
    s_accept = true;
    s_rec_n = 0;
    CK(runlog_pump(1000U) == RUNLOG_WINDOW, "runlog_refused_items_published_once_link_accepts");
    /* one pass is bounded by the window even with a huge backlog */
    CK(runlog_pump(1001U) == 0U && s_rec_n == (int)RUNLOG_WINDOW, "runlog_pump_pass_is_bounded");
    s_up = false;
    t0 = esp_timer_get_time();
    for (uint32_t i = 0U; i < 1000U; i++) (void)runlog_pump(i);
    CK(esp_timer_get_time() - t0 < 1000000, "runlog_pump_offline_returns_immediately");
}

/* ---- telemetry integration: one whole run through the real builders ---------- */
static bool parse_u(const char *body, const char *key, uint32_t *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    *out = (uint32_t)strtoul(p + strlen(needle), NULL, 10);
    return true;
}

static void test_run_cycle(void)
{
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    telemetry_client_test_set_flags(true, false);
    rl_reset();

    dispense_job_t job;
    memset(&job, 0, sizeof(job));
    job.id = 7U; job.material_id = 1U; job.channel_id = 1U; job.target_g = 5000;
    job.remote_command_id = 555U; job.state = JOB_COMPLETE; job.final_g = 5002;
    job.start_g = 10; job.finish_ms = 6000U;
    dual_channel_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.profile_id, sizeof(snap.profile_id), "tune-a");
    snap.profile_version = 3U;

    uint32_t seq = telemetry_client_test_run_cycle(1, &job, &snap, 1000U, 12U);
    CK(seq == 6U, "run_cycle_batches_share_one_counter");   /* ASSIGNED, STARTED, STATE_CHANGE + 3 sample batches */

    char run_id[70] = "";
    uint32_t next = 0U;
    bool start_first = false, complete_last = false, in_order = true;
    int starts = 0, completes = 0, last_suffix_complete = 0;
    char complete_body[460] = "";
    char start_body[460] = "";
    for (uint32_t t = 0U; t < 100U; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        for (int i = 0; i < s_rec_n; i++) {
            const rec_t *r = &s_rec[i];
            if (!strcmp(r->suffix, "run/start")) {
                starts++;
                snprintf(start_body, sizeof(start_body), "%s", r->body);
                if (t == 0U && i == 0) start_first = true;
                const char *p = strstr(r->body, "\"run_id\":\"");
                if (p) { p += 10; const char *e = strchr(p, '"'); snprintf(run_id, sizeof(run_id), "%.*s", (int)(e - p), p); }
                ack(run_id, "START_OK", 0);
                last_suffix_complete = 0;
            } else if (!strcmp(r->suffix, "run/complete")) {
                completes++;
                snprintf(complete_body, sizeof(complete_body), "%s", r->body);
                last_suffix_complete = 1;
                ack(run_id, "COMPLETE", -1);
            } else {
                if (r->batch_seq != next + 1U) in_order = false;
                next = r->batch_seq;
                ack(run_id, "BATCH_OK", r->batch_seq);
                last_suffix_complete = 0;
            }
        }
    }
    complete_last = last_suffix_complete == 1;
    CK(start_first && starts == 1 && completes == 1 && complete_last && in_order, "run_cycle_start_batches_complete_order");
    CK(strstr(start_body, "\"boot_id\":\"") && strstr(start_body, "\"message_id\":\"") &&
       strstr(start_body, "\"command_id\":555") && strstr(start_body, "\"profile_id\":\"tune-a\"") &&
       strstr(start_body, "\"profile_version\":3") && strstr(start_body, "\"config\":{"), "run_cycle_start_body_fields");
    uint32_t end_seq = 0U, tb = 0U, ts = 0U, te = 0U, dr = 99U;
    bool have = parse_u(complete_body, "end_seq", &end_seq) && parse_u(complete_body, "total_batches", &tb) &&
                parse_u(complete_body, "total_samples", &ts) && parse_u(complete_body, "total_events", &te) &&
                parse_u(complete_body, "dropped_samples", &dr);
    CK(have && end_seq == 7U && tb == 7U && ts == 12U && te == 4U && dr == 0U && end_seq == next,
       "run_cycle_complete_counts_match_batches");
    CK(strstr(complete_body, "\"status\":\"COMPLETE\"") && strstr(complete_body, "\"final_weight_g\":5002") &&
       strstr(complete_body, "\"message_id\":\""), "run_cycle_complete_fields");
    CK(runlog_pending() == 0U, "run_cycle_everything_acked_ring_empty");

    /* overflow: 100 samples -> the local ring and the run ring both drop, and say so */
    rl_reset();
    job.id = 8U;
    (void)telemetry_client_test_run_cycle(2, &job, &snap, 20000U, 100U);
    next = 0U;
    uint32_t dropped_total = 0U, total_samples = 0U;
    uint32_t ring_dropped = 0U;
    complete_body[0] = '\0';
    run_id[0] = '\0';
    for (uint32_t t = 0U; t < 200U; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        for (int i = 0; i < s_rec_n; i++) {
            const rec_t *r = &s_rec[i];
            if (!strcmp(r->suffix, "run/start")) {
                const char *p = strstr(r->body, "\"run_id\":\"");
                if (p) { p += 10; const char *e = strchr(p, '"'); snprintf(run_id, sizeof(run_id), "%.*s", (int)(e - p), p); }
                ring_dropped = runlog_dropped_samples(run_id);
                ack(run_id, "START_OK", 0);
            } else if (!strcmp(r->suffix, "run/complete")) {
                snprintf(complete_body, sizeof(complete_body), "%s", r->body);
                ack(run_id, "COMPLETE", -1);
            } else {
                ack(run_id, "BATCH_OK", r->batch_seq);
            }
        }
    }
    (void)parse_u(complete_body, "dropped_samples", &dropped_total);
    (void)parse_u(complete_body, "total_samples", &total_samples);
    CK(total_samples == 100U && dropped_total >= 52U && dropped_total == 52U + ring_dropped,
       "run_cycle_overflow_dropped_samples_counted_in_complete");

    telemetry_client_test_set_flags(false, false);
    rl_reset();
}

static void test_ack_gating(void)
{
    const char *R = "dev-ch1-j9-900-0009";
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 10);
    (void)runlog_pump(0U);
    telemetry_client_test_set_flags(false, false);
    telemetry_client_run_ack("{\"run_id\":\"dev-ch1-j9-900-0009\",\"acked_batch_seq\":0,\"state\":\"START_OK\"}", 79U);
    CK(runlog_pending() == 1U, "run_ack_ignored_while_flag_off");
    telemetry_client_test_set_flags(true, false);
    telemetry_client_run_ack("{\"run_id\":\"dev-ch1-j9-900-0009\",\"acked_batch_seq\":0,\"state\":\"START_OK\"}", 79U);
    CK(runlog_pending() == 0U, "run_ack_trims_when_flag_on");
    telemetry_client_test_set_flags(false, false);
}

/* ---- profile handshake ------------------------------------------------------- */
static int s_tp_n;
static bool t_connected(void) { return true; }
static bool t_publish(const char *suffix, const char *json, int qos, bool retain)
{
    (void)qos; (void)retain;
    if (s_tp_n >= 6) return false;
    snprintf(s_tp[s_tp_n].suffix, sizeof(s_tp[s_tp_n].suffix), "%s", suffix);
    snprintf(s_tp[s_tp_n].body, sizeof(s_tp[s_tp_n].body), "%s", json);
    s_tp_n++;
    return true;
}
static const telemetry_mqtt_ops_t s_tops = { .connected = t_connected, .publish = t_publish, .rx_pop = NULL };

static int s_http_n;
static char s_http_state[12], s_http_error[100];
static void http_ack(void *ctx, uint32_t id, const char *state, uint32_t lj, uint8_t ch, const char *error)
{
    (void)ctx; (void)id; (void)lj; (void)ch;
    s_http_n++;
    snprintf(s_http_state, sizeof(s_http_state), "%s", state);
    snprintf(s_http_error, sizeof(s_http_error), "%s", error);
}

static mqtt_link_queue_t s_q;

static const char *job_json(uint32_t id, const char *extra, const char *pid, uint32_t pver, const char *nid, uint32_t nver)
{
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": %lu, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": null, "
        "\"ttl_ms\": 0, %s\"material_id\": \"M1\", \"target_g\": 5000, \"priority\": 0, "
        "\"profile_id\": \"%s\", \"profile_version\": %lu, "
        "\"profile\": {\"profile_id\": \"%s\", \"version\": %lu, \"kp\": 0.1100, \"ki\": 0.0, "
        "\"kd\": 0.0, \"tolerance_g\": 20, \"max_overshoot_g\": 100, \"max_duration_ms\": 120000, "
        "\"window_ms\": 500, \"min_on_ms\": 40, \"min_off_ms\": 40}}",
        (unsigned long)id, extra, pid, (unsigned long)pver, nid, (unsigned long)nver);
    return s_js;
}

static void send(const char *json, uint32_t t)
{
    static const char topic[] = "cas/relay-test/commands";
    size_t n = strlen(json);
    if (mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), json, n, n, false, t, esp_timer_get_time()) != MQTT_ENQ_OK) return;
    if (!mqtt_link_queue_pop_rx(&s_q, &s_item, 0U)) return;
    (void)telemetry_client_mqtt_intake(s_item.data, s_item.len, false, s_item.recv_ms, s_item.recv_us);
    (void)telemetry_client_mqtt_step(t + 1U);
}

static uint32_t jobs_queued(void)
{
    dispense_job_t *jobs = calloc(JOB_QUEUE_MAX, sizeof(*jobs));   /* heap: static DRAM is full */
    if (jobs == NULL) return UINT32_MAX;
    uint32_t n = job_queue_snapshot(jobs, JOB_QUEUE_MAX);
    free(jobs);
    return n;
}

static void ctl_setup(void)
{
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    telemetry_client_test_reset_commands();
    s_tp_n = 0;
    memset(s_tp, 0, sizeof(s_tp));
}

static void test_profile_handshake(void)
{
    static const char *HASH = "00564fdacd7459a8";   /* python sha256 of the canonical profile, first 16 hex */
    CK(!telemetry_client_run_mqtt_enabled() && !telemetry_client_require_pin() && !telemetry_client_hash_check_enabled(),
       "flags_default_off");

    uint8_t d[32];
    telemetry_client_test_sha256((const uint8_t *)"abc", 3U, d);
    CK(d[0] == 0xbaU && d[1] == 0x78U && d[2] == 0x16U && d[3] == 0xbfU && d[28] == 0xf2U && d[29] == 0x00U &&
       d[30] == 0x15U && d[31] == 0xadU,
       "sha256_matches_fips_abc_vector");
    uint8_t d2[32];
    static uint8_t big[200];
    memset(big, 'a', sizeof(big));
    telemetry_client_test_sha256(big, 55U, d);    /* 55 and 56 bytes straddle the padding boundary */
    telemetry_client_test_sha256(big, 56U, d2);
    CK(memcmp(d, d2, 32) != 0, "sha256_padding_boundary_distinct");

    char h[17], h2[17];
    CK(telemetry_client_job_profile_hash(job_json(1, "", "tune-a", 1, "tune-a", 1), h) && !strcmp(h, HASH),
       "profile_hash_matches_reference");
    /* same profile, other key order and compact spacing: same hash */
    const char *reordered =
        "{\"command_id\":2,\"profile\":{\"window_ms\":500,\"version\":1,\"tolerance_g\":20,\"profile_id\":\"tune-a\","
        "\"min_on_ms\":40,\"min_off_ms\":40,\"max_overshoot_g\":100,\"max_duration_ms\":120000,\"kp\":0.1100,"
        "\"ki\":0.0,\"kd\":0.0}}";
    CK(telemetry_client_job_profile_hash(reordered, h2) && !strcmp(h2, HASH), "profile_hash_order_and_spacing_independent");
    CK(telemetry_client_job_profile_hash(job_json(3, "", "tune-a", 1, "tune-a", 1), h) &&
       telemetry_client_job_profile_hash(job_json(3, "", "tune-a", 2, "tune-a", 2), h2) && strcmp(h, h2) != 0,
       "profile_hash_changes_with_profile");
    CK(!telemetry_client_job_profile_hash("{\"command_id\":4}", h) && !telemetry_client_job_profile_hash(NULL, h),
       "profile_hash_absent_profile_is_false");

    ctl_setup();
    mqtt_link_queue_init(&s_q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH);
    telemetry_client_set_mqtt_ops(&s_tops);
    uint32_t t = 500000U;

    /* F4: all flags 0 (the default): a JOB with a MISMATCHING profile_hash behaves as before 9.3 */
    send(job_json(3999, "\"profile_hash\": \"0123456789abcdef\", ", "tune-a", 1, "tune-a", 1), t - 100U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"state\":\"QUEUED\"") && !strstr(s_tp[0].body, "PROFILE_HASH") &&
       !strstr(s_tp[0].body, "applied_profile") && jobs_queued() == 1U, "hash_check_off_mismatching_hash_ignored_job_queued");
    char dpid[65]; uint32_t dver = 0; char dhash[17];
    CK(!telemetry_client_test_applied_profile(3999U, dpid, sizeof(dpid), &dver, dhash),
       "hash_check_off_no_applied_profile_recorded");
    ctl_setup();
    s_tp_n = 0;
    telemetry_client_test_set_hash_check(true);   /* the 9.3 checks below need the flag */

    /* ACK reports what the relay staged */
    send(job_json(4001, "", "tune-a", 1, "tune-a", 1), t);
    CK(s_tp_n == 1 && !strcmp(s_tp[0].suffix, "commands/ack") && strstr(s_tp[0].body, "\"state\":\"QUEUED\"") &&
       strstr(s_tp[0].body, "\"applied_profile\":{\"profile_id\":\"tune-a\",\"version\":1,\"hash\":\"00564fdacd7459a8\"}"),
       "ack_carries_applied_profile_id_version_hash");
    char pid[65]; uint32_t ver = 0; char hh[17];
    CK(telemetry_client_test_applied_profile(4001U, pid, sizeof(pid), &ver, hh) && !strcmp(pid, "tune-a") && ver == 1U &&
       !strcmp(hh, HASH) && jobs_queued() == 1U, "applied_profile_read_back_from_queued_job");
    /* duplicate delivery: same outcome, same profile, no second job */
    s_tp_n = 0;
    send(job_json(4001, "", "tune-a", 1, "tune-a", 1), t + 10U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"applied_profile\":{\"profile_id\":\"tune-a\",\"version\":1,\"hash\":\"00564fdacd7459a8\"}") &&
       jobs_queued() == 1U, "duplicate_reack_keeps_applied_profile");

    /* a matching profile_hash is accepted, a wrong one refuses the JOB */
    s_tp_n = 0;
    send(job_json(4002, "\"profile_hash\": \"00564FDACD7459A8\", ", "tune-a", 1, "tune-a", 1), t + 20U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"state\":\"QUEUED\"") && jobs_queued() == 2U, "matching_profile_hash_accepted_case_insensitive");
    s_tp_n = 0;
    send(job_json(4003, "\"profile_hash\": \"0123456789abcdef\", ", "tune-a", 1, "tune-a", 1), t + 30U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"state\":\"FAILED\"") && strstr(s_tp[0].body, "PROFILE_HASH_MISMATCH") &&
       !strstr(s_tp[0].body, "applied_profile") && jobs_queued() == 2U, "wrong_profile_hash_refused_no_applied_profile");
    s_tp_n = 0;
    send(job_json(4004, "\"profile_hash\": \"abc\", ", "tune-a", 1, "tune-a", 1), t + 40U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "PROFILE_HASH_MISMATCH") && jobs_queued() == 2U, "malformed_profile_hash_refused");
    /* other refusals never claim a profile */
    s_tp_n = 0;
    send(job_json(4005, "", "tune-a", 3, "tune-a", 4), t + 50U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"state\":\"FAILED\"") && !strstr(s_tp[0].body, "applied_profile"),
       "refused_pin_has_no_applied_profile");

    /* require_pinned_profile: default OFF keeps the legacy deferral; ON refuses */
    static const char legacy[] =
        "{\"command_id\": 4101, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": null, "
        "\"ttl_ms\": 0, \"material_id\": \"M1\", \"target_g\": 5000, \"priority\": 0}";
    ctl_setup();
    s_tp_n = 0;
    telemetry_client_test_set_flags(false, false);
    send(legacy, t + 100U);
    telemetry_cmd_stats_t st;
    telemetry_client_cmd_stats(&st);
    CK(s_tp_n == 0 && st.deferred_to_http == 1U && jobs_queued() == 0U, "require_pin_off_legacy_job_still_deferred");
    telemetry_client_test_set_flags(false, true);
    send(legacy, t + 110U);
    telemetry_client_cmd_stats(&st);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"state\":\"FAILED\"") && strstr(s_tp[0].body, "PROFILE_REQUIRED") &&
       st.deferred_to_http == 1U && jobs_queued() == 0U && !strstr(s_tp[0].body, "applied_profile"),
       "require_pin_on_refuses_job_without_pin_over_mqtt");
    /* HTTP path: refused too, and without the blocking profile fetch (which would need a server) */
    static char legacy2[sizeof(legacy) + 8];
    snprintf(legacy2, sizeof(legacy2), "%s", legacy);
    legacy2[sizeof("{\"command_id\": 410") - 1U] = '3';   /* command_id 4103: a fresh id, same pin-less shape */
    s_http_n = 0;
    telemetry_client_dispatch_command(legacy2, TELEMETRY_CMD_HTTP, t, t + 120U, http_ack, NULL);
    CK(s_http_n == 1 && !strcmp(s_http_state, "FAILED") && strstr(s_http_error, "PROFILE_REQUIRED") && jobs_queued() == 0U,
       "require_pin_on_refuses_job_without_pin_over_http");
    send(job_json(4102, "", "tune-a", 1, "tune-a", 1), t + 130U);
    CK(jobs_queued() == 1U && strstr(s_tp[s_tp_n - 1].body, "\"state\":\"QUEUED\""), "require_pin_on_pinned_job_still_queued");
    telemetry_client_test_set_flags(false, false);

    telemetry_client_test_set_hash_check(false);
    telemetry_client_set_mqtt_ops(NULL);
    mqtt_link_queue_deinit(&s_q);
    ctl_setup();
}

/* ---- review fixes F1..F6 --------------------------------------------------- */
_Static_assert(RUNLOG_ITEM_MAX <= MQTT_LINK_TX_PAYLOAD_MAX, "run ring item must fit the MQTT link payload");

static bool m_rx_pop(telemetry_mqtt_rx_t *out, uint32_t t)
{
    if (!mqtt_link_queue_pop_rx(&s_q, &s_item, t)) return false;
    out->len = s_item.len;
    out->recv_ms = s_item.recv_ms;
    out->recv_us = s_item.recv_us;
    memcpy(out->data, s_item.data, (size_t)s_item.len + 1U);
    return true;
}
static bool m_ack_pop(char *out, size_t cap, size_t *len, uint32_t t)
{
    if (!mqtt_link_queue_pop_ack(&s_q, &g->aitem, t) || (size_t)g->aitem.len >= cap) return false;
    memcpy(out, g->aitem.data, (size_t)g->aitem.len + 1U);
    *len = g->aitem.len;
    return true;
}
static const telemetry_mqtt_ops_t s_ops2 = { .connected = t_connected, .publish = t_publish,
                                             .rx_pop = m_rx_pop, .run_ack_pop = m_ack_pop };

static void evt(const char *topic, const char *json, uint32_t t)
{
    size_t n = strlen(json);
    (void)mqtt_link_queue_push_event(&s_q, NULL, "cas/relay-test/run/ack", topic, strlen(topic), json, n, n, false,
                                     t, esp_timer_get_time());
}
static const char *stop_json(uint32_t id)
{
    snprintf(s_body, sizeof(s_body),
        "{\"command_id\": %lu, \"type\": \"PUMP_STOP\", \"command_type\": \"PUMP_STOP\", \"channel_id\": \"CH1\", \"ttl_ms\": 0}",
        (unsigned long)id);
    return s_body;
}

/* F1 + F5: [JOB, run/ack, STOP] keeps STOP-first; the ack never touches mqtt_cmd; commands survive a failed run alloc. */
static void test_ack_isolation(void)
{
    static const char CT[] = "cas/relay-test/commands", AT[] = "cas/relay-test/run/ack";
    const char *R = "dev-ch1-j5-500-0005";
    ctl_setup();
    CK(mqtt_link_queue_init(&s_q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH) && mqtt_link_queue_ack_init(&s_q, MQTT_LINK_ARX_DEPTH),
       "ack_queue_created");
    telemetry_client_set_mqtt_ops(&s_ops2);
    telemetry_client_test_set_flags(true, false);
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 10);
    (void)runlog_pump(0U);
    uint32_t t = 600000U;

    evt(CT, job_json(5001, "", "tune-a", 1, "tune-a", 1), t);
    evt(AT, "{\"run_id\":\"dev-ch1-j5-500-0005\",\"acked_batch_seq\":0,\"state\":\"START_OK\"}", t);
    evt(CT, stop_json(5003), t);
    CK(mqtt_link_queue_pending_rx(&s_q) == 2U && uxQueueMessagesWaiting(s_q.arx) == 1U, "ack_routed_to_own_queue_not_rx");
    uint32_t locks = runlog_lock_takes();
    CK(telemetry_client_mqtt_drain(0U) == 2U && telemetry_client_mqtt_staged() == 2U,
       "drain_takes_JOB_and_STOP_ack_does_not_end_it");
    (void)telemetry_client_mqtt_step(t + 1U);
    CK(s_tp_n == 2 && strstr(s_tp[0].body, "\"command_id\":5003") && strstr(s_tp[1].body, "\"command_id\":5001") &&
       jobs_queued() == 1U, "stop_first_with_ack_between_job_and_stop");
    CK(runlog_lock_takes() == locks && runlog_pending() == 1U, "mqtt_cmd_path_never_takes_run_ring_lock");
    CK(telemetry_client_run_ack_drain() == 1U && runlog_pending() == 0U && runlog_lock_takes() > locks,
       "run_task_applies_ack_under_ring_lock");

    for (uint32_t i = 0U; i < MQTT_LINK_ARX_DEPTH + 3U; i++) evt(AT, "{\"run_id\":\"x\",\"state\":\"COMPLETE\"}", t);
    CK(s_q.arx_dropped_full == 3U && mqtt_link_queue_pending_rx(&s_q) == 0U, "ack_flood_drops_and_counts_never_touches_rx");
    CK(telemetry_client_run_ack_drain() == 6U, "ack_drain_is_bounded");
    (void)telemetry_client_run_ack_drain();

    /* F5: run-history allocation failure only disables run tracking */
    CK(!telemetry_client_test_run_tracking_init(true) && !telemetry_client_run_tracking_enabled() &&
       !telemetry_client_run_mqtt_enabled() && telemetry_client_run_alloc_failures() >= 1U,
       "run_alloc_failure_disables_run_tracking_counted");
    /* N1: ring allocation failure falls back to HTTP instead of losing items */
    uint32_t af = telemetry_client_run_alloc_failures();
    telemetry_client_test_set_flags(true, false);
    CK(!telemetry_client_test_run_mqtt_ring_init(true) && !telemetry_client_run_mqtt_enabled() &&
       telemetry_client_run_alloc_failures() == af + 1U &&
       !runlog_add(RUNLOG_START, "r1", 0U, 0U, "{}", 2U), "run_ring_alloc_failure_falls_back_to_http_counted");
    telemetry_client_test_set_flags(true, false);
    CK(telemetry_client_test_run_mqtt_ring_init(false) && telemetry_client_run_mqtt_enabled() &&
       runlog_add(RUNLOG_START, "r1", 0U, 0U, "{}", 2U), "run_ring_init_ok_keeps_mqtt");
    runlog_init(NULL);
    telemetry_client_test_set_flags(false, false);
    s_tp_n = 0;
    evt(CT, stop_json(5010), t + 10U);
    CK(telemetry_client_mqtt_drain(0U) == 1U, "commands_still_drained_without_run_tracking");
    (void)telemetry_client_mqtt_step(t + 11U);
    CK(s_tp_n == 1 && strstr(s_tp[0].body, "\"command_id\":5010"), "remote_stop_still_acked_without_run_tracking");

    telemetry_client_set_mqtt_ops(NULL);
    mqtt_link_queue_deinit(&s_q);
    telemetry_client_test_set_flags(false, false);
    ctl_setup();
}

/* F2: run items have their own outbox budget; command ACKs always keep the reserve. */
static size_t s_ob;
static int ob_enqueue(void *ctx, const char *topic, const char *data, int len, int qos, bool retain)
{
    (void)ctx; (void)topic; (void)data; (void)qos; (void)retain;
    if (s_ob + (size_t)len > 8192U) return -1;     /* esp-mqtt outbox limit */
    s_ob += (size_t)len;
    return 1;
}
static size_t ob_size(void *ctx) { (void)ctx; return s_ob; }

static void test_outbox_budget(void)
{
    mqtt_link_tx_ops_t ops = { .ctx = NULL, .enqueue = ob_enqueue, .outbox_size = ob_size };
    mqtt_link_queue_t q;
    memset(&q, 0, sizeof(q));
    char *body = malloc(2400U);
    if (body == NULL) { CK(false, "outbox_budget_alloc"); return; }
    memset(body, 'a', 2399U); body[2399] = '\0';
    mqtt_tx_item_t run = { .qos = 1, .retain = false, .len = 2399U, .data = body };
    snprintf(run.topic, sizeof(run.topic), "cas/dev/run/samples");
    mqtt_tx_item_t ack = { .qos = 1, .retain = false, .len = 600U, .data = body };
    snprintf(ack.topic, sizeof(ack.topic), "cas/dev/commands/ack");
    CK(mqtt_link_topic_is_run("cas/dev/run/start") && mqtt_link_topic_is_run(run.topic) &&
       !mqtt_link_topic_is_run(ack.topic) && !mqtt_link_topic_is_run("cas/dev/status") && !mqtt_link_topic_is_run(NULL),
       "run_topic_classifier");

    /* half-open link: no PUBACK ever drains the outbox, the ring resends again and again */
    s_ob = 0U;
    int enq = 0, deferred = 0;
    bool within = true;
    for (int resend = 0; resend < 40; resend++) {
        mqtt_tx_result_t r = mqtt_link_tx_dispatch(&ops, &q, &run, true, 8192U);
        if (r == MQTT_TX_ENQUEUED) enq++; else if (r == MQTT_TX_RUN_DEFERRED) deferred++; else within = false;
        if (s_ob + MQTT_LINK_ACK_RESERVE > 8192U) within = false;
    }
    CK(within && enq == 2 && deferred == 38 && q.tx_deferred_run == 38U, "run_resends_stop_at_budget_reserve_kept");
    int acks = 0;
    for (int i = 0; i < 3; i++) if (mqtt_link_tx_dispatch(&ops, &q, &ack, true, 8192U) == MQTT_TX_ENQUEUED) acks++;
    CK(acks == 3 && s_ob <= 8192U, "command_acks_enqueue_after_run_resend_storm");
    s_ob = 0U;
    free(body);
}

/* F3: total ring cap, oversize refusal, start/events never dropped while capacity remains. */
static bool add_len(runlog_kind_t k, const char *run, uint32_t seq, uint32_t ns, size_t len, char *buf)
{
    int n = snprintf(buf, 64U, "{\"run_id\":\"%s\",\"batch_seq\":%lu,\"x\":\"", run, (unsigned long)seq);
    memset(buf + n, 'b', len - (size_t)n - 2U);
    buf[len - 2U] = '"';
    buf[len - 1U] = '}';
    buf[len] = '\0';
    return runlog_add(k, run, seq, ns, buf, len);
}

static void test_ring_caps(void)
{
    const char *R = "dev-ch1-j6-600-0006";
    char *buf = malloc(RUNLOG_ITEM_MAX + 400U);
    if (buf == NULL) { CK(false, "ring_caps_alloc"); return; }
    runlog_stats_t st;
    rl_reset();
    CK(!add_len(RUNLOG_EVENTS, R, 1, 0, RUNLOG_ITEM_MAX + 1U, buf), "oversize_item_refused_at_enqueue");
    runlog_stats(&st);
    CK(st.add_oversize == 1U && st.add_refused == 1U && runlog_pending() == 0U, "oversize_counted_ring_untouched");
    CK(add_len(RUNLOG_EVENTS, R, 1, 0, RUNLOG_ITEM_MAX, buf), "max_size_item_accepted");

    rl_reset();
    CK(add_len(RUNLOG_START, R, 0, 0, 200, buf), "caps_start_added");
    for (uint32_t i = 0U; i < 4U; i++) CK(add_len(RUNLOG_SAMPLES, R, 1U + i, 5U, 4000, buf), "caps_samples_added");
    uint32_t ev_ok = 0U;
    bool within = true;
    for (uint32_t i = 0U; i < 10U; i++) {
        if (add_len(RUNLOG_EVENTS, R, 10U + i, 0, 4000, buf)) ev_ok++;
        runlog_stats(&st);
        if (st.total_bytes > RUNLOG_TOTAL_BYTES) within = false;
    }
    runlog_stats(&st);
    CK(within && ev_ok == 6U && st.dropped_batches == 4U && st.dropped_samples == 20U && st.refused_total_cap == 4U,
       "total_cap_drops_samples_first_then_refuses_events");
    CK(runlog_dropped_samples(R) == 20U, "total_cap_drops_counted_per_run");
    uint32_t crit_ok = 0U;
    for (uint32_t i = 0U; i < 4U; i++) if (add_len(RUNLOG_COMPLETE, R, 100U + i, 0, 1500, buf)) crit_ok++;
    runlog_stats(&st);
    CK(crit_ok >= 1U && crit_ok < 4U && st.total_bytes <= RUNLOG_TOTAL_BYTES + RUNLOG_CRITICAL_RESERVE,
       "critical_reserve_bounded_never_grows_unbounded");

    /* nothing accepted is ever lost: every start/event/complete reaches the wire */
    uint32_t seen = 0U, want = 1U + ev_ok + crit_ok;
    for (uint32_t t = 0U; t < 300U; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        for (int i = 0; i < s_rec_n; i++) {
            if (strcmp(s_rec[i].suffix, "run/samples") != 0) seen++;
            ack(R, "BATCH_OK", s_rec[i].batch_seq);
        }
    }
    CK(seen == want, "accepted_start_events_complete_all_published");
    free(buf);
    rl_reset();
}

/* F6: structural run/ack parse. */
static void test_ack_parser_hostile(void)
{
    const char *R = "dev-ch1-j7-700-0007";
    rl_reset();
    (void)add(RUNLOG_START, R, 0, 0, 10);
    (void)runlog_pump(0U);
    const char *nested = "{\"meta\":{\"run_id\":\"dev-ch1-j7-700-0007\",\"state\":\"COMPLETE\"},\"run_id\":\"other\","
                         "\"state\":\"BATCH_OK\",\"acked_batch_seq\":9}";
    runlog_on_ack(nested, strlen(nested));
    CK(runlog_pending() == 1U, "ack_key_inside_nested_object_not_matched");
    const char *instr = "{\"note\":\"\\\"run_id\\\":\\\"x\\\",\\\"state\\\":\\\"REJECTED\\\"\",\"run_id\":\"other\","
                        "\"state\":\"REJECTED\"}";
    runlog_on_ack(instr, strlen(instr));
    CK(runlog_pending() == 1U, "ack_key_spelled_in_string_value_not_matched");
    const char *dup = "{\"run_id\":\"dev-ch1-j7-700-0007\",\"run_id\":\"dev-ch1-j7-700-0007\",\"state\":\"REJECTED\"}";
    runlog_on_ack(dup, strlen(dup));
    const char *bad = "{\"run_id\":\"dev-ch1-j7-700-0007\",\"state\":\"BATCH_OK\",\"acked_batch_seq\":\"9\"}";
    runlog_on_ack(bad, strlen(bad));
    const char *trunc = "{\"run_id\":\"dev-ch1-j7-700-0007\",\"state\":\"REJECTED\"";
    runlog_on_ack(trunc, strlen(trunc));
    CK(runlog_pending() == 1U, "ack_duplicate_key_wrong_type_or_truncated_ignored");
    const char *good = " {\"run_id\": \"dev-ch1-j7-700-0007\" , \"acked_batch_seq\" : 0 ,\"state\": \"START_OK\"}";
    runlog_on_ack(good, strlen(good));
    CK(runlog_pending() == 0U, "ack_valid_with_whitespace_still_applied");
    rl_reset();
}

/* ---- fourth review round (N1..N4) ------------------------------------------------ */
#define s_be (g->be)
static bool s_be_silent;

static void be_reset(const char *a, long poison_a, const char *b, long poison_b)
{
    memset(s_be, 0, sizeof(s_be));
    snprintf(s_be[0].id, sizeof(s_be[0].id), "%s", a);
    s_be[0].poison = poison_a;
    if (b) { snprintf(s_be[1].id, sizeof(s_be[1].id), "%s", b); s_be[1].poison = poison_b; }
    s_be_silent = false;
}

/* The backend sees what the last pump published and answers like CONTRACT 9.2/9.10. */
static void be_feed(uint32_t now)
{
    for (int i = 0; i < s_rec_n; i++) {
        const rec_t *r = &s_rec[i];
        be_t *b = NULL;
        for (int k = 0; k < 2; k++) {
            char pat[32];
            snprintf(pat, sizeof(pat), "\"run_id\":\"%s\"", s_be[k].id);
            if (s_be[k].id[0] && strstr(r->body, pat)) b = &s_be[k];
        }
        if (b == NULL) continue;
        bool start = !strcmp(r->suffix, "run/start"), comp = !strcmp(r->suffix, "run/complete");
        bool tomb = strstr(r->body, "\"dropped\":true") != NULL;
        if (start) {
            if (!b->started) { b->started = true; b->applied++; b->next = 1U; } else b->dups++;
            ack(b->id, "START_OK", (long)b->next - 1);
        } else if (!b->started) {
            ack(b->id, "UNKNOWN_RUN", -1);
        } else if (r->batch_seq < b->next) {
            b->dups++;
            if (b->closed) ack(b->id, "COMPLETE", -1); else ack(b->id, "BATCH_OK", (long)b->next - 1);
        } else if (r->batch_seq == b->next) {
            if (!comp && !tomb && b->poison == (long)r->batch_seq) continue;   /* write fails: no ack, no store */
            b->applied++;
            b->next++;
            if (comp) { b->closed = true; b->complete_ms = now; ack(b->id, "COMPLETE", -1); }
            else { if (tomb) b->tombs++; ack(b->id, "BATCH_OK", (long)b->next - 1); }
        } else {
            ack(b->id, "BATCH_OK", (long)b->next - 1);   /* gap: only the watermark before it */
        }
    }
}

static void drive(uint32_t from, uint32_t to, uint32_t step)
{
    for (uint32_t t = from; t <= to; t += step) {
        s_rec_n = 0;
        (void)runlog_pump(t);
        if (!s_be_silent) be_feed(t);
    }
}

static void add_run_a(void)   /* START 0, EVENT 1, SAMPLES 2 3, COMPLETE 4 */
{
    (void)add(RUNLOG_START, "ra", 0, 0, 20);
    (void)add(RUNLOG_EVENTS, "ra", 1, 0, 20);
    (void)add(RUNLOG_SAMPLES, "ra", 2, 5, 100);
    (void)add(RUNLOG_SAMPLES, "ra", 3, 5, 100);
    (void)add(RUNLOG_COMPLETE, "ra", 4, 0, 20);
}
static void add_run_b(void)   /* START 0, SAMPLES 1 2, COMPLETE 3 */
{
    (void)add(RUNLOG_START, "rb", 0, 0, 20);
    (void)add(RUNLOG_SAMPLES, "rb", 1, 5, 100);
    (void)add(RUNLOG_SAMPLES, "rb", 2, 5, 100);
    (void)add(RUNLOG_COMPLETE, "rb", 3, 0, 20);
}

/* total backend silence of `silent` ms starting after `pre` ms of normal service, then it answers */
static void silence_case(uint32_t pre, uint32_t silent, const char *name)
{
    char n[80];
    runlog_stats_t st;
    rl_reset();
    be_reset("ra", -1, "rb", -1);
    add_run_a();
    add_run_b();
    if (pre) drive(0U, pre, 2000U);
    uint32_t pend = runlog_pending(), pend_a = runlog_pending_for_run("ra");
    s_be_silent = true;
    drive(pre ? pre + 2000U : 0U, pre + silent, 5000U);
    runlog_stats(&st);
    snprintf(n, sizeof(n), "%s_nothing_abandoned_or_released_during_silence", name);
    CK(st.resend_abandoned == 0U && st.tombstones == 0U && st.abandoned_items == 0U && runlog_pending() == pend &&
       runlog_pending_for_run("ra") == pend_a, n);
    s_be_silent = false;
    drive(pre + silent + 2000U, pre + silent + 600000U, 2000U);
    runlog_stats(&st);
    snprintf(n, sizeof(n), "%s_reconciles_in_order_no_loss_no_dup_no_wedge", name);
    CK(s_be[0].closed && s_be[1].closed && s_be[0].applied == 5U && s_be[1].applied == 4U && runlog_pending() == 0U &&
       st.tombstones == 0U && st.resend_abandoned == 0U && st.dropped_batches == 0U, n);
}

static void test_review4(void)
{
    runlog_stats_t st;

    /* N1: silence before START_OK, mid-run, 700 s and 3600 s */
    silence_case(0U, 700000U, "silent_700s_before_start_ok");
    silence_case(4000U, 700000U, "silent_700s_mid_run");
    silence_case(0U, 3600000U, "silent_3600s_before_start_ok");
    silence_case(4000U, 3600000U, "silent_3600s_mid_run");

    /* N1c/e: the backend is alive but never stores ra seq 1 (no ack): only that item is tombstoned,
     * the later items and the OTHER run complete without waiting for it */
    rl_reset();
    be_reset("ra", 2, "rb", -1);
    (void)add(RUNLOG_START, "ra", 0, 0, 20);
    (void)add(RUNLOG_EVENTS, "ra", 1, 0, 20);
    (void)add(RUNLOG_SAMPLES, "ra", 2, 5, 100);   /* poison */
    (void)add(RUNLOG_SAMPLES, "ra", 3, 5, 100);
    (void)add(RUNLOG_COMPLETE, "ra", 4, 0, 20);
    add_run_b();
    drive(0U, 2000000U, 5000U);
    runlog_stats(&st);
    CK(st.tombstones == 1U && st.resend_abandoned == 1U && st.dropped_samples == 5U && s_be[0].tombs == 1U &&
       s_be[0].closed && s_be[0].applied == 5U && runlog_pending() == 0U, "alive_backend_one_unacked_item_tombstoned_run_completes");
    CK(s_be[1].closed && s_be[1].applied == 4U && s_be[1].complete_ms <= 60000U, "other_run_completes_while_head_is_stuck");

    /* N1e: fairness, a long run does not starve a short one (old FIFO: rb waited for all of ra) */
    rl_reset();
    be_reset("ra", -1, "rb", -1);
    (void)add(RUNLOG_START, "ra", 0, 0, 20);
    for (uint32_t i = 1U; i <= 14U; i++) (void)add(RUNLOG_EVENTS, "ra", i, 0, 20);
    (void)add(RUNLOG_COMPLETE, "ra", 15, 0, 20);
    add_run_b();
    s_rec_n = 0;
    (void)runlog_pump(0U);
    CK(s_rec_n == 2 && strstr(s_rec[0].body, "\"ra\"") && strstr(s_rec[1].body, "\"rb\""), "fair_first_window_serves_both_runs");
    be_feed(0U);
    drive(1000U, 200000U, 1000U);
    CK(s_be[0].closed && s_be[1].closed && s_be[1].complete_ms < s_be[0].complete_ms && s_be[0].applied == 16U &&
       s_be[1].applied == 4U && runlog_pending() == 0U, "fair_short_run_not_starved_order_kept_no_gap");

    /* N2: tombstone body is written in place; a refused shrink still gives a valid, ackable tombstone */
    rl_reset();
    runlog_test_fail_tomb_shrink(true);
    be_reset("ra", -1, NULL, -1);
    (void)add(RUNLOG_START, "ra", 0, 0, 20);
    for (uint32_t i = 1U; i <= 4U; i++) (void)add(RUNLOG_SAMPLES, "ra", i, 5, 5);      /* bodies far below a tombstone */
    for (uint32_t i = 5U; i <= 13U; i++) (void)add(RUNLOG_SAMPLES, "ra", i, 5, 1950);
    (void)add(RUNLOG_COMPLETE, "ra", 14, 0, 20);
    runlog_stats(&st);
    CK(st.tombstones >= 4U, "tomb_shrink_refused_tombstones_created");
    drive(0U, 400000U, 1000U);
    runlog_test_fail_tomb_shrink(false);
    CK(s_be[0].closed && s_be[0].applied == 15U && s_be[0].tombs >= 4U && runlog_pending() == 0U,
       "tomb_shrink_refused_still_ackable_no_gap");

    /* N3: a late UNKNOWN_RUN of the previous cycle (pump re-sent in between) is not a new cycle */
    rl_reset();
    (void)add(RUNLOG_START, "ra", 0, 0, 20);
    (void)add(RUNLOG_EVENTS, "ra", 1, 0, 20);
    CK(runlog_pump(0U) == 2U, "unk_cycle_setup");
    for (uint32_t c = 1U; c <= RUNLOG_MAX_UNKNOWN; c++) {
        ack("ra", "UNKNOWN_RUN", -1);                 /* reply 1 of cycle c (counts) */
        CK(runlog_pump(c) == 2U, "unk_pump_resends_after_reply");
        ack("ra", "UNKNOWN_RUN", -1);                 /* reply 2 of cycle c, after the re-send: absorbed */
        CK(runlog_pump(c + 1000U) == 0U, "unk_late_reply_does_not_unsend_the_new_generation");
    }
    CK(runlog_pending() == 2U, "unk_late_replies_do_not_add_up_to_abandonment");
    ack("ra", "UNKNOWN_RUN", -1);                     /* a genuine 4th cycle */
    CK(runlog_pending() == 0U, "unk_genuine_cycles_still_abandon");

    /* N4: ring full of other things: the active run is superseded, never silently overwritten */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    telemetry_client_test_set_flags(true, false);
    rl_reset();
    s_up = false;                                   /* broker down for long */
    dispense_job_t job;
    memset(&job, 0, sizeof(job));
    job.id = 31U; job.material_id = 1U; job.channel_id = 1U; job.target_g = 5000; job.start_g = 10;
    dual_channel_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.profile_id, sizeof(snap.profile_id), "tune-a");
    telemetry_client_test_run_begin(1, &job, &snap, 1000U);
    CK(runlog_pending() == 3U, "n4_first_run_start_and_two_events_queued");
    for (uint32_t i = 1U; i < 400U; i++) (void)add(RUNLOG_EVENTS, "zz", i, 0, 10);   /* fill to the generic reserve */
    uint32_t before = runlog_pending();
    job.id = 32U;                                   /* next job takes the slot while run 31 is un-closed */
    telemetry_client_test_run_begin(1, &job, &snap, 5000U);
    runlog_stats(&st);
    CK(st.superseded_runs == 1U && st.superseded_unclosed == 0U && runlog_pending() == before + 2U,
       "n4_unclosed_run_closed_as_unknown_and_new_start_has_reserved_slot");
    s_up = true;
    bool saw_super = false, saw_new_start = false, super_end_ok = false;
    for (uint32_t t = 0U; t < 100U; t++) {
        s_rec_n = 0;
        (void)runlog_pump(t * 1000U);
        for (int i = 0; i < s_rec_n; i++) {
            const rec_t *r = &s_rec[i];
            char rid[70] = "";
            const char *p = strstr(r->body, "\"run_id\":\"");
            if (p) { p += 10; const char *e = strchr(p, '"'); snprintf(rid, sizeof(rid), "%.*s", (int)(e - p), p); }
            if (!strcmp(r->suffix, "run/complete") && strstr(r->body, "RUN_SUPERSEDED")) {
                saw_super = true;
                super_end_ok = strstr(r->body, "\"end_seq\":2") && strstr(r->body, "\"status\":\"FAILED\"") && strstr(r->body, "-j31-");
                ack(rid, "COMPLETE", -1);
            } else if (!strcmp(r->suffix, "run/start")) {
                if (strstr(r->body, "\"job_id\":32")) saw_new_start = true;
                ack(rid, "START_OK", 0);
            } else if (!strcmp(r->suffix, "run/complete")) ack(rid, "COMPLETE", -1);
            else ack(rid, "BATCH_OK", r->batch_seq);
        }
    }
    CK(saw_super && super_end_ok && saw_new_start, "n4_superseded_complete_and_new_start_published");

    /* complete refused too (ring completely full): counted as unclosed, nothing overwritten silently */
    rl_reset();
    s_up = false;
    job.id = 41U;
    telemetry_client_test_run_begin(2, &job, &snap, 1000U);
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) (void)add(RUNLOG_COMPLETE, "zz", 100U + i, 0, 10);
    job.id = 42U;
    telemetry_client_test_run_begin(2, &job, &snap, 2000U);
    runlog_stats(&st);
    CK(st.superseded_runs == 1U && st.superseded_unclosed == 1U && runlog_pending() == RUNLOG_SLOTS,
       "n4_superseded_complete_refused_is_counted_unclosed");
    telemetry_client_test_set_flags(false, false);
    rl_reset();
}

static volatile bool s_task_done;

/* Own task: the run builders and the JOB dispatch are deeper than the main task's stack. */
static void run_mqtt_test_task(void *arg)
{
    (void)arg;
    test_ring();
    test_overflow();
    test_never_block();
    test_run_cycle();
    test_ack_gating();
    test_profile_handshake();
    test_ack_isolation();
    test_outbox_budget();
    test_ring_caps();
    test_ack_parser_hostile();
    test_ack_minus_one();
    test_tombstones();
    test_review3();
    test_review4();
    s_task_done = true;
    vTaskDelete(NULL);
}

void test_run_mqtt_run(void)
{
    g = calloc(1U, sizeof(*g));
    if (g == NULL) { CK(false, "run_mqtt_test_context_alloc"); return; }
    s_task_done = false;
    if (xTaskCreate(run_mqtt_test_task, "run_mqtt_t", 10240, NULL, 2, NULL) != pdPASS) {
        CK(false, "run_mqtt_test_task_created");
        free(g);
        g = NULL;
        return;
    }
    for (int i = 0; i < 600 && !s_task_done; i++) vTaskDelay(pdMS_TO_TICKS(100));
    CK(s_task_done, "run_mqtt_tests_finished");
    runlog_init(NULL);
    telemetry_client_test_set_flags(false, false);
    if (s_task_done) free(g);
    g = NULL;
}