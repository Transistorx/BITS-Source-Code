/*
 * Independent safety review fixes for the sender as a DISPENSING weight source.
 *
 * H1  one protocol only (CI-2001), lock while ONLINE, no generic fallback,
 *     resync to the next terminator after ANY flush/overflow/stall, stable
 *     never inherited from a previous frame.
 * M1  OL (overload) is never published as a weight.
 * M2  strict pad option, decimal-format plausibility guard.
 * L   reader-stall flush, summary buffer worst case, RAW CAPTURE.
 *
 * Every reader here runs the production configuration (one protocol, no
 * auto-detect, no generic fallback) unless a test says otherwise.
 */

#include "test_reader_safety.h"

#include "test_harness.h"

#include "cas_ci2001_parser.h"
#include "log_util.h"
#include "mock_transport.h"
#include "scale_events.h"
#include "scale_manager.h"
#include "scale_reader.h"
#include "weight_source.h"

#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
#include "asuki_k1_parser.h"
#include "asuki_parser.h"
#include "cas_parser.h"
#include "flintec_parser.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define WIRE 22U

static const scale_protocol_t *k_ci_only[] = { &cas_ci2001_protocol };

typedef struct {
    bool strict_pad;
    uint8_t pad_value;
    uint32_t stall_ms;
    bool autodetect;
    bool generic;
} opts_t;

typedef struct {
    mock_transport_t mock;
    QueueHandle_t events;
    QueueHandle_t forward;   /* optional: scale_manager queue (as CH1) */
    unsigned weight_readings;
    unsigned status_only_overload;
    unsigned online_events;
    scale_reading_t last_weight;
} rig_t;

static void make_wire(uint8_t out[WIRE], const char *status, const char *gn,
                      uint8_t id, uint8_t lamp, const char *weight8, uint8_t pad,
                      const char *unit)
{
    out[0] = (uint8_t)status[0];
    out[1] = (uint8_t)status[1];
    out[2] = ',';
    out[3] = (uint8_t)gn[0];
    out[4] = (uint8_t)gn[1];
    out[5] = ',';
    out[6] = id;
    out[7] = lamp;
    out[8] = ',';
    memcpy(&out[9], weight8, 8U);
    out[17] = pad;
    out[18] = (uint8_t)unit[0];
    out[19] = (uint8_t)unit[1];
    out[20] = '\r';
    out[21] = '\n';
}

static bool rig_start(rig_t *r, uint8_t channel_id, const opts_t *o)
{
    memset(r, 0, sizeof(*r));
    mock_transport_init(&r->mock);
    const scale_link_config_t link_cfg = {
        .uart_port = 0, .rx_gpio = -1, .tx_gpio = -1, .baud_rate = 9600,
    };
    if (scale_transport_open(&r->mock.base, &link_cfg) != ESP_OK) return false;
    r->events = xQueueCreate(64, sizeof(scale_event_t));
    if (r->events == NULL) return false;

    scale_reader_config_t cfg = {
        .channel_id = channel_id,
        .transport = &r->mock.base,
        .protocols = k_ci_only,
        .num_protocols = 1U,
        .event_queue = r->events,
    };
    if (o != NULL) {
        cfg.strict_pad = o->strict_pad;
        cfg.pad_value = o->pad_value;
        cfg.stall_gap_ms = o->stall_ms;
        cfg.allow_auto_detect = o->autodetect;
        cfg.allow_generic_fallback = o->generic;
#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
        if (o->autodetect) {
            static const scale_protocol_t *all[] = {
                &asuki_protocol, &asuki_k1_protocol, &flintec_protocol,
                &cas_protocol, &cas_ci2001_protocol,
            };
            cfg.protocols = all;
            cfg.num_protocols = sizeof(all) / sizeof(all[0]);
        }
#endif
    }
    if (scale_reader_start(&cfg) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(40));
    return true;
}

static void rig_collect(rig_t *r)
{
    scale_event_t ev;
    while (xQueueReceive(r->events, &ev, 0) == pdTRUE) {
        if (ev.type == SCALE_EVENT_ONLINE) r->online_events++;
        if (ev.type == SCALE_EVENT_READING) {
            if (ev.reading.has_display || ev.reading.has_gross) {
                r->weight_readings++;
                r->last_weight = ev.reading;
            } else if (ev.reading.has_status && ev.reading.overload) {
                r->status_only_overload++;
            }
        }
        if (r->forward != NULL) {
            ev.channel_id = 0U;               /* present as CH1 to the manager */
            ev.reading.channel_id = 0U;
            (void)xQueueSend(r->forward, &ev, 0);
        }
    }
    if (r->forward != NULL) vTaskDelay(pdMS_TO_TICKS(40));
}

static void rig_feed(rig_t *r, const uint8_t *data, size_t len)
{
    (void)mock_transport_push_bytes(&r->mock, data, len);
    vTaskDelay(pdMS_TO_TICKS(80));
    rig_collect(r);
}

static void rig_online(rig_t *r)
{
    uint8_t w[WIRE];
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(r, w, WIRE);
    rig_feed(r, w, WIRE);
}

static void rig_reset_counts(rig_t *r)
{
    r->weight_readings = 0U;
    r->status_only_overload = 0U;
    r->online_events = 0U;
}

/* ---- H1: truncated tail after overflow / generic fallback absence ------- */

static void test_tail_after_overflow(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 5U, NULL)) { test_check(false, "rs_tail_rig_start"); return; }
    rig_online(&rig);
    test_check(rig.weight_readings == 1U && rig.online_events == 1U, "rs_tail_online_ci2001_only");
    rig_reset_counts(&rig);

    /* Frame cut in half by a FIFO overflow; the flush loses the middle. */
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    (void)mock_transport_push_bytes(&rig.mock, w, 12U);
    (void)mock_transport_push_event(&rig.mock, SCALE_XPORT_EV_FIFO_OVF);
    vTaskDelay(pdMS_TO_TICKS(80));
    rig_collect(&rig);
    scale_reader_get_diagnostics(5U, &d);
    test_check(d.fifo_overflows == 1U && d.resync_events >= 1U, "rs_tail_overflow_resyncs");

    /* The tail of that frame, as decoded text ' 12.345' -> '2.345 kg'. */
    const uint8_t tail1[] = { '2', '.', '3', '4', '5', ' ', 'k', 'g', '\r', '\n' };
    rig_feed(&rig, tail1, sizeof(tail1));
    test_check(rig.weight_readings == 0U, "rs_tail_truncated_tail_never_published");
    scale_reader_get_diagnostics(5U, &d);
    test_check(d.valid_frames == 2U && d.invalid_frames == 0U,
               "rs_tail_discarded_without_parsing");

    /* After the resync the stream is frame aligned again. */
    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 1U, "rs_tail_next_frame_accepted");

    /* A tail that would pass the OLD generic fallback, arriving ONLINE with no
     * overflow at all: a CI-2001-only reader must reject it. */
    rig_reset_counts(&rig);
    const uint8_t generic[] = " 12.345 kg\r\n";
    rig_feed(&rig, generic, sizeof(generic) - 1U);
    const uint8_t generic2[] = "2.345 kg\r\n";
    rig_feed(&rig, generic2, sizeof(generic2) - 1U);
    scale_reader_get_diagnostics(5U, &d);
    test_check(rig.weight_readings == 0U, "rs_generic_fallback_absent_online");
    test_check(d.invalid_frames == 2U && d.rej_length == 2U, "rs_generic_lines_rejected_by_length");

    /* BUFFER_FULL (driver ring dropped bytes) also resyncs. */
    uint32_t resyncs = d.resync_events;
    (void)mock_transport_push_event(&rig.mock, SCALE_XPORT_EV_BUFFER_FULL);
    vTaskDelay(pdMS_TO_TICKS(80));
    scale_reader_get_diagnostics(5U, &d);
    test_check(d.resync_events == resyncs + 1U, "rs_buffer_full_resyncs");
}

/* ---- H1: merged frames ---------------------------------------------------- */

static void test_merged_frames(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 6U, NULL)) { test_check(false, "rs_merge_rig_start"); return; }
    rig_online(&rig);
    rig_reset_counts(&rig);

    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    uint8_t merged[20U + WIRE];
    memcpy(merged, w, 20U);              /* first frame lost its CR LF */
    memcpy(merged + 20U, w, WIRE);
    rig_feed(&rig, merged, sizeof(merged));
    scale_reader_get_diagnostics(6U, &d);
    test_check(rig.weight_readings == 0U, "rs_merged_frames_never_published");
    test_check(d.rej_length == 1U, "rs_merged_frames_rejected_by_length");

    uint8_t merged_cr[21U + WIRE];       /* first frame lost only its LF */
    memcpy(merged_cr, w, 21U);
    memcpy(merged_cr + 21U, w, WIRE);
    rig_feed(&rig, merged_cr, sizeof(merged_cr));
    test_check(rig.weight_readings == 0U, "rs_merged_cr_frames_never_published");

    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 1U, "rs_merged_then_clean_frame_accepted");
}

/* ---- H1: lock while ONLINE / production config validation ----------------- */

static void test_protocol_lock_and_config(void)
{
    scale_reader_diagnostics_t d;

    /* Production validation: exactly one protocol, no legacy flags. */
    static mock_transport_t m;
    mock_transport_init(&m);
    const scale_link_config_t lc = { .uart_port = 0, .rx_gpio = -1, .tx_gpio = -1, .baud_rate = 9600 };
    (void)scale_transport_open(&m.base, &lc);
    QueueHandle_t q = xQueueCreate(4, sizeof(scale_event_t));
    static const scale_protocol_t *two[] = { &cas_ci2001_protocol, &cas_ci2001_protocol };
    scale_reader_config_t bad = {
        .channel_id = 7U, .transport = &m.base, .protocols = two, .num_protocols = 2U,
        .event_queue = q,
    };
    test_check(scale_reader_start(&bad) == ESP_ERR_INVALID_ARG, "rs_cfg_two_protocols_without_autodetect_refused");
    bad.num_protocols = 1U;
    bad.allow_generic_fallback = true;
    test_check(scale_reader_start(&bad) == ESP_ERR_INVALID_ARG, "rs_cfg_generic_without_autodetect_refused");

#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
    /* Legacy opt-in, generic ASCII enabled: it works, and once ONLINE the
     * reader is locked to the generic parser (a CI-2001 frame is ignored). */
    static rig_t rig;
    opts_t o = { .autodetect = true, .generic = true };
    if (!rig_start(&rig, 7U, &o)) { test_check(false, "rs_lock_rig_start"); return; }
    const uint8_t line[] = " 1.234 kg\r\n";
    rig_feed(&rig, line, sizeof(line) - 1U);
    rig_feed(&rig, line, sizeof(line) - 1U);
    test_check(rig.weight_readings == 1U, "rs_legacy_generic_opt_in_works_when_enabled");
    rig_reset_counts(&rig);
    uint8_t w[WIRE];
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(7U, &d);
    test_check(rig.weight_readings == 0U, "rs_locked_generic_ignores_other_protocol_frames");

    /* Auto-detect WITHOUT generic: locks to CI-2001, generic lines never pass. */
    static rig_t rig2;
    opts_t o2 = { .autodetect = true, .generic = false };
    if (!rig_start(&rig2, 8U, &o2)) { test_check(false, "rs_lock2_rig_start"); return; }
    rig_online(&rig2);
    test_check(rig2.weight_readings == 1U, "rs_legacy_autodetect_locks_to_ci2001");
    rig_reset_counts(&rig2);
    rig_feed(&rig2, line, sizeof(line) - 1U);
    rig_feed(&rig2, line, sizeof(line) - 1U);
    test_check(rig2.weight_readings == 0U, "rs_locked_ci2001_rejects_generic_lines");
#else
    (void)d;
#endif
}

/* ---- M1: overload ---------------------------------------------------------- */

static void test_overload(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 9U, NULL)) { test_check(false, "rs_ol_rig_start"); return; }
    rig_online(&rig);
    rig_reset_counts(&rig);
    scale_reader_get_diagnostics(9U, &d);
    uint32_t valid_before = d.valid_frames;

    make_wire(w, "OL", "GS", 1U, 0x00, "9999.999", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(9U, &d);
    test_check(rig.weight_readings == 0U, "rs_ol_frame_not_published_as_weight");
    test_check(rig.status_only_overload == 1U, "rs_ol_reported_status_only");
    test_check(d.overload_frames == 1U && d.valid_frames == valid_before &&
               d.invalid_frames == 0U, "rs_ol_counted_as_overload_not_valid");
    test_check(d.ol_captured && d.ol_raw_len == 20U && d.ol_raw[0] == 'O' && d.ol_raw[1] == 'L',
               "rs_ol_first_raw_frame_captured");

    /* The capture is once per boot: a later OL does not overwrite it. */
    make_wire(w, "OL", "NT", 1U, 0x00, "8888.888", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(9U, &d);
    test_check(d.overload_frames == 2U && d.ol_raw[3] == 'G', "rs_ol_capture_only_first_frame");

    /* OL alone does not keep the link alive: it drops with reason overload. */
    for (int i = 0; i < 17; i++) {
        rig_feed(&rig, w, WIRE);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    scale_reader_get_diagnostics(9U, &d);
    test_check(!d.link_online && d.link_reason == SCALE_LINK_REASON_TIMEOUT_OVERLOAD,
               "rs_ol_link_drops_with_reason_overload");
    test_check(strstr(scale_link_reason_name(SCALE_LINK_REASON_TIMEOUT_OVERLOAD), "overload") != NULL,
               "rs_ol_reason_name_says_overload");
    test_check(rig.weight_readings == 0U, "rs_ol_stream_never_published");

    /* OL frames can never confirm a link on their own. */
    static rig_t rig2;
    if (!rig_start(&rig2, 10U, NULL)) { test_check(false, "rs_ol2_rig_start"); return; }
    rig_feed(&rig2, w, WIRE);
    rig_feed(&rig2, w, WIRE);
    rig_feed(&rig2, w, WIRE);
    scale_reader_get_diagnostics(10U, &d);
    test_check(!d.link_online && rig2.online_events == 0U && rig2.weight_readings == 0U,
               "rs_ol_frames_cannot_confirm_link");
}

/* ---- M2: strict pad and format guard --------------------------------------- */

static void test_pad_and_format(void)
{
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    /* Default: pad is NOT enforced, but the observed pad/lamp are captured. */
    static rig_t rig0;
    if (!rig_start(&rig0, 11U, NULL)) { test_check(false, "rs_pad0_rig_start"); return; }
    make_wire(w, "ST", "GS", 1U, 0x37, "0010.000", 0x41, "kg");
    rig_feed(&rig0, w, WIRE);
    rig_feed(&rig0, w, WIRE);
    scale_reader_get_diagnostics(11U, &d);
    test_check(rig0.weight_readings == 1U && d.rej_pad == 0U, "rs_pad_not_enforced_by_default");
    test_check(d.have_pad_lamp && d.last_pad == 0x41U && d.last_lamp == 0x37U,
               "rs_pad_and_lamp_of_last_valid_frame_captured");

    /* strict_pad = 0x41: any other pad byte is rejected, before and after ONLINE. */
    static rig_t rig1;
    opts_t o = { .strict_pad = true, .pad_value = 0x41U };
    if (!rig_start(&rig1, 12U, &o)) { test_check(false, "rs_pad1_rig_start"); return; }
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig1, w, WIRE);
    rig_feed(&rig1, w, WIRE);
    scale_reader_get_diagnostics(12U, &d);
    test_check(d.rej_pad == 2U && d.valid_frames == 0U && !d.link_online,
               "rs_strict_pad_rejects_wrong_pad_offline");
    test_check(d.invalid_frames == 2U, "rs_strict_pad_counted_as_invalid");
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x41, "kg");
    rig_feed(&rig1, w, WIRE);
    rig_feed(&rig1, w, WIRE);
    scale_reader_get_diagnostics(12U, &d);
    test_check(d.link_online && rig1.weight_readings == 1U, "rs_strict_pad_accepts_expected_pad");
    rig_reset_counts(&rig1);
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x42, "kg");
    rig_feed(&rig1, w, WIRE);
    scale_reader_get_diagnostics(12U, &d);
    test_check(rig1.weight_readings == 0U && d.rej_pad == 3U, "rs_strict_pad_rejects_wrong_pad_online");

    /* Format guard (decimal point position / digit count). */
    static rig_t rig;
    if (!rig_start(&rig, 13U, NULL)) { test_check(false, "rs_fmt_rig_start"); return; }
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.00 ", 0x00, "kg");   /* 2 decimals */
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(13U, &d);
    test_check(!d.link_online, "rs_fmt_mismatching_candidates_do_not_confirm_link");
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 1U, "rs_fmt_established_3_decimals");
    rig_reset_counts(&rig);

    make_wire(w, "ST", "GS", 1U, 0x00, "010.0000", 0x00, "kg");   /* 4 decimals */
    rig_feed(&rig, w, WIRE);
    make_wire(w, "ST", "GS", 1U, 0x00, "00010000", 0x00, "kg");   /* no decimal point */
    rig_feed(&rig, w, WIRE);
    make_wire(w, "ST", "GS", 1U, 0x00, "  10.00 ", 0x00, "kg");   /* 2 decimals */
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(13U, &d);
    test_check(rig.weight_readings == 0U && d.rej_format == 3U, "rs_fmt_mismatch_rejected_as_format");
    test_check(d.rej_length + d.rej_header + d.rej_status + d.rej_field + d.rej_other +
                   d.rej_format + d.rej_pad == d.invalid_frames,
               "rs_fmt_reject_reasons_sum_to_invalid_frames");

    make_wire(w, "ST", "GS", 1U, 0x00, " 123.456", 0x00, "kg");   /* same format, other value */
    rig_feed(&rig, w, WIRE);
    make_wire(w, "US", "NT", 1U, 0x00, "-001.500", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 2U, "rs_fmt_same_format_any_value_accepted");
}

/* ---- L(a): reader stall -------------------------------------------------- */

static void test_reader_stall(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    opts_t o = { .stall_ms = 120U };
    if (!rig_start(&rig, 14U, &o)) { test_check(false, "rs_stall_rig_start"); return; }
    rig_online(&rig);
    rig_reset_counts(&rig);
    scale_reader_get_diagnostics(14U, &d);
    test_check(d.reader_stalls == 0U, "rs_stall_none_in_normal_operation");

    /* The reader task does not run for 400 ms while frames pile up in the ring. */
    rig.mock.block_once_ms = 400U;
    vTaskDelay(pdMS_TO_TICKS(60));      /* reader enters the blocked wait */
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    (void)mock_transport_push_bytes(&rig.mock, w, WIRE);
    (void)mock_transport_push_bytes(&rig.mock, w, WIRE);
    vTaskDelay(pdMS_TO_TICKS(600));
    rig_collect(&rig);
    scale_reader_get_diagnostics(14U, &d);
    test_check(d.reader_stalls == 1U && d.last_stall_gap_ms >= 300U, "rs_stall_detected_and_counted");
    test_check(rig.weight_readings == 0U, "rs_stall_late_stamped_frames_never_published");
    test_check(rig.mock.flush_calls >= 1U && d.dropped_bytes >= 2U * WIRE - 4U,
               "rs_stall_flushes_ring_and_counts_dropped_bytes");
    test_check(d.resync_events >= 1U, "rs_stall_resyncs_to_terminator");

    /* After the stall the first frame only resyncs, the next one is accepted. */
    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 0U, "rs_stall_first_frame_after_flush_is_sacrificed");
    rig_feed(&rig, w, WIRE);
    test_check(rig.weight_readings == 1U, "rs_stall_recovers_on_following_frame");
}

/* ---- L(b)(c): summary buffer and RAW CAPTURE ------------------------------ */

static void test_log_formats(void)
{
    char b[LOG_CHAN_SUMMARY_MAX];
    log_chan_t c;
    memset(&c, 0xFF, sizeof(c));         /* every counter at its maximum */
    c.ch = 2; c.uart = 2; c.selected = true; c.has_weight = true; c.stable = true;
    c.have_age = true; c.have_seq = true; c.weight_g = -2147483647 - 1;
    c.link = "OFFLINE";
    size_t n = log_fmt_chan_summary(b, sizeof(b), &c);
    test_check(n > 0U && n < LOG_CHAN_SUMMARY_MAX, "rs_summary_worst_case_fits_buffer");
    test_check(strstr(b, "fmt=4294967295") != NULL && strstr(b, "resync=4294967295") != NULL &&
               strstr(b, "elf=4294967295") != NULL, "rs_summary_new_counters_present");
    printf("SUMMARY_WORST_CASE_LEN=%u\n", (unsigned)n);
    test_check(LOG_CHAN_SUMMARY_MAX >= 512U, "rs_summary_buffer_is_512");

    uint8_t tail[LOG_RAW_TAIL_MAX];
    for (size_t i = 0; i < sizeof(tail); i++) tail[i] = (uint8_t)(0xF0U + (i & 0x0FU));
    log_raw_t r = { .ch = 1, .uart = 1, .tail = tail, .tail_len = sizeof(tail),
                    .have_pad_lamp = true, .pad = 0x41, .lamp = 0x0A, .embedded_lf = 4294967295U };
    char line[LOG_RAW_LINE_MAX];
    n = log_fmt_raw_capture(line, sizeof(line), &r);
    test_check(n > 0U && n < LOG_RAW_LINE_MAX && strstr(line, "last=40B") != NULL &&
               strstr(line, "pad=0x41 lamp=0x0A") != NULL, "rs_raw_capture_40_bytes_fits");

    const uint8_t s[] = { 'S', 'T', ',', 0x0A, '"', 0x00 };
    r.tail = s; r.tail_len = sizeof(s); r.have_pad_lamp = false; r.embedded_lf = 2U;
    n = log_fmt_raw_capture(line, sizeof(line), &r);
    test_check(n > 0U && strstr(line, "hex=53 54 2C 0A 22 00") != NULL &&
               strstr(line, "ascii=\"ST,..") != NULL && strstr(line, "pad=n/a lamp=n/a elf=2") != NULL,
               "rs_raw_capture_hex_and_printable_ascii");
    r.tail_len = 0U;
    test_check(log_fmt_raw_capture(line, sizeof(line), &r) > 0U && strstr(line, "hex=none") != NULL,
               "rs_raw_capture_empty_tail");
    r.tail_len = LOG_RAW_TAIL_MAX + 1U;
    test_check(log_fmt_raw_capture(line, sizeof(line), &r) == 0U, "rs_raw_capture_oversize_refused");
    test_check(log_fmt_raw_capture(line, 20U, &r) == 0U, "rs_raw_capture_truncation_reported");

    const uint8_t ol[] = { 'O', 'L', ',', 'G', 'S', ',', 0x01, 0x00, ',', '9', '9', '9', '9', '.',
                           '9', '9', '9', 0x00, 'k', 'g' };
    n = log_fmt_raw_ol(line, sizeof(line), 1, 1, ol, sizeof(ol));
    test_check(n > 0U && strstr(line, "first-OL-frame=20B") != NULL &&
               strstr(line, "hex=4F 4C 2C 47 53") != NULL, "rs_raw_ol_format");
    test_check(log_fmt_raw_ol(line, sizeof(line), 1, 1, NULL, 0U) == 0U, "rs_raw_ol_null_safe");
}

static void test_raw_capture_reader(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 15U, NULL)) { test_check(false, "rs_raw_rig_start"); return; }
    scale_reader_get_diagnostics(15U, &d);
    test_check(d.raw_tail_len == 0U && !d.have_pad_lamp && !d.ol_captured, "rs_raw_empty_at_start");

    make_wire(w, "ST", "GS", 0x0A, 0x0A, "0010.000", 0x00, "kg");  /* id and lamp are LF */
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(15U, &d);
    test_check(d.raw_tail_len == WIRE && memcmp(d.raw_tail, w, WIRE) == 0, "rs_raw_tail_accumulates_short_chunks");
    test_check(d.embedded_lf == 2U, "rs_raw_embedded_lf_counted");

    uint8_t w2[WIRE];
    make_wire(w2, "US", "GS", 1U, 0x37, "0005.500", 0x41, "kg");
    rig_feed(&rig, w2, WIRE);
    scale_reader_get_diagnostics(15U, &d);
    test_check(d.raw_tail_len == SCALE_RAW_TAIL_SIZE &&
               memcmp(d.raw_tail, w + (WIRE - (SCALE_RAW_TAIL_SIZE - WIRE)), SCALE_RAW_TAIL_SIZE - WIRE) == 0 &&
               memcmp(d.raw_tail + (SCALE_RAW_TAIL_SIZE - WIRE), w2, WIRE) == 0,
               "rs_raw_tail_keeps_last_40_bytes");
    test_check(d.have_pad_lamp && d.last_pad == 0x41U && d.last_lamp == 0x37U,
               "rs_raw_pad_lamp_of_last_valid_frame");

    uint8_t big[100];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i + 1U);
    rig_feed(&rig, big, sizeof(big));
    scale_reader_get_diagnostics(15U, &d);
    test_check(d.raw_tail_len == SCALE_RAW_TAIL_SIZE &&
               memcmp(d.raw_tail, big + 60U, SCALE_RAW_TAIL_SIZE) == 0, "rs_raw_tail_from_large_chunk");
}

/* ---- End to end as CH1 through scale_manager and weight_source -------------- */

static void send_status_less_weight(QueueHandle_t manager, double kg)
{
    scale_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.channel_id = 0U;
    ev.type = SCALE_EVENT_READING;
    ev.reading = (scale_reading_t){
        .valid = true, .has_display = true, .display = kg, .display_is_physical = true,
        .has_gross = true, .gross = kg, .gross_is_physical = true, .has_unit = true,
        .timestamp_ms = weight_source_now_ms(), .channel_id = 0U,
    };
    strcpy(ev.reading.unit, "kg");
    (void)xQueueSend(manager, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
}

static void test_ch1_end_to_end(QueueHandle_t manager)
{
    static rig_t rig;
    weight_sample_t s;
    uint8_t w[WIRE];

    if (manager == NULL) return;
    weight_source_start();
    weight_source_select_channel(1U);

    /* Production configuration (CI-2001 only, channel 1): the reader's events
     * reach scale_manager as CH1 and weight_source publishes from there. */
    if (!rig_start(&rig, 16U, NULL)) { test_check(false, "rs_e2e_rig_start"); return; }
    rig.forward = manager;

    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.channel == 1U &&
               s.weight_g == 10000 && s.stable, "rs_e2e_ch1_stable_10kg_published");

    /* Overload: no weight, distinct state, and the previous weight is not reused. */
    make_wire(w, "OL", "GS", 1U, 0x00, "9999.999", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_NONE, "rs_e2e_overload_publishes_nothing");
    test_check(weight_source_overload_active() && weight_source_overload_events() == 1U,
               "rs_e2e_overload_counted_once");
    weight_source_log_cas_status(weight_source_now_ms());
    (void)weight_source_get(&s);
    test_check(weight_source_overload_events() == 1U, "rs_e2e_overload_not_recounted_per_poll");

    /* Recovery from OL: the next normal frame publishes again, status of ITS frame. */
    make_wire(w, "US", "GS", 1U, 0x00, "0005.500", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.weight_g == 5500 && !s.stable &&
               !weight_source_overload_active(), "rs_e2e_overload_cleared_by_next_frame");

    /* Stable must never be inherited: ST frame, then a weight with NO status. */
    make_wire(w, "ST", "GS", 1U, 0x00, "0007.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.weight_g == 7000 && s.stable,
               "rs_e2e_stable_frame_is_stable");
    send_status_less_weight(manager, 7.5);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.weight_g == 7500 && !s.stable,
               "rs_e2e_status_less_frame_does_not_inherit_stable");

    /* And a status-only OL event after a stable weight blocks it at once. */
    make_wire(w, "ST", "GS", 1U, 0x00, "0007.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.stable, "rs_e2e_stable_again");
    make_wire(w, "OL", "NT", 1U, 0x00, "9999.999", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_NONE, "rs_e2e_overload_after_stable_blocks_at_once");

    /* weight_sample_from_reading refuses an overload/error reading outright. */
    scale_reading_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.valid = true; rd.has_display = true; rd.display = 1.0; rd.display_is_physical = true;
    rd.has_unit = true; strcpy(rd.unit, "kg"); rd.has_status = true; rd.overload = true;
    test_check(!weight_sample_from_reading(&rd, 1U, &s, NULL), "rs_sample_from_overload_reading_refused");
    rd.overload = false; rd.stable = true;
    test_check(weight_sample_from_reading(&rd, 1U, &s, NULL) && s.stable, "rs_sample_stable_from_same_frame_status");
    rd.has_status = false;
    test_check(weight_sample_from_reading(&rd, 1U, &s, NULL) && !s.stable,
               "rs_sample_without_status_is_not_stable");

    weight_source_select_channel(0U);
    weight_source_start();
}

void test_reader_safety_run(QueueHandle_t manager_queue)
{
    test_log_formats();
    test_tail_after_overflow();
    test_merged_frames();
    test_protocol_lock_and_config();
    test_pad_and_format();
    test_overload();
    test_reader_stall();
    test_raw_capture_reader();
    test_ch1_end_to_end(manager_queue);
}
