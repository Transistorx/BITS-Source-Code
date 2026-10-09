#include "test_transport_golden.h"

#include "test_harness.h"

#include "asuki_k1_parser.h"
#include "asuki_parser.h"
#include "cas_ci2001_parser.h"
#include "cas_parser.h"
#include "flintec_parser.h"
#include "mock_transport.h"
#include "scale_events.h"
#include "scale_reader.h"
#include "scale_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Golden frames were captured from the pre-refactor RS232 path. See
 * docs/superpowers/baseline/rs232-golden/GOLDEN_BASELINE.md and
 * golden_frames.h. Nothing here may be "fixed" to accommodate a behavioural
 * change — the frames and their expected outputs are the contract.
 */

/* Pull the golden table in; it is header-only by design so it cannot drift
 * from a separately compiled copy. */
#include "golden_frames.h"

/* The production auto-detect table, in production order. Using the full list
 * proves no other parser claims a CAS frame. */
static const scale_protocol_t *k_protocols[] = {
    &asuki_protocol,
    &asuki_k1_protocol,
    &flintec_protocol,
    &cas_protocol,
    &cas_ci2001_protocol,
};

static bool nearly_equal(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

/* Compare every parser-produced field against the frozen expectation.
 * channel_id and timestamp_ms are stamped by the reader after parsing and are
 * therefore not part of the golden contract. */
static void assert_reading_matches_golden(const scale_reading_t *rd,
                                          const golden_frame_t *g,
                                          const char *prefix)
{
    char name[128];

#define GOLD_CHECK(cond, suffix)                                      \
    do {                                                              \
        (void)snprintf(name, sizeof(name), "%s_%s", prefix, suffix);  \
        test_check((cond), name);                                     \
    } while (0)

    GOLD_CHECK(rd->valid == g->expect_valid, "valid");
    GOLD_CHECK(rd->has_gross == g->expect_has_gross, "has_gross");
    GOLD_CHECK(rd->has_net == g->expect_has_net, "has_net");
    GOLD_CHECK(rd->has_display == g->expect_has_display, "has_display");
    GOLD_CHECK(rd->stable == g->expect_stable, "stable");
    GOLD_CHECK(rd->overload == g->expect_overload, "overload");
    GOLD_CHECK(nearly_equal(rd->gross, g->expect_gross), "gross");
    GOLD_CHECK(nearly_equal(rd->net, g->expect_net), "net");
    GOLD_CHECK(nearly_equal(rd->display, g->expect_display), "display");
    GOLD_CHECK(rd->has_unit && strcmp(rd->unit, g->expect_unit) == 0, "unit");
    GOLD_CHECK(rd->raw_status == g->expect_raw_status, "raw_status");

#undef GOLD_CHECK
}

/* ---- 1. Pure parser output, no framing, no transport -------------------
 * This is the tightest form of "same frames -> same parser outputs". If any
 * of these fail after a refactor, the refactor changed the parser. */
static void test_golden_parser_outputs(void)
{
    for (size_t i = 0U; i < GOLDEN_FRAME_COUNT; i++) {
        const golden_frame_t *g = &k_golden_frames[i];
        scale_reading_t rd;

        /* The parser sees the body only: the reader strips CR LF first. */
        bool parsed = cas_ci2001_protocol.parse_frame_with_length(
            g->wire, GOLDEN_BODY_SIZE, &rd);

        char prefix[96];
        (void)snprintf(prefix, sizeof(prefix), "parser_%s", g->name);
        test_check(parsed, prefix);

        if (parsed) {
            assert_reading_matches_golden(&rd, g, prefix);
        }
    }
}

/* ---- 2. Frozen negatives: every malformed body must stay rejected ------ */
static void test_golden_parser_rejects_malformed(void)
{
    scale_reading_t rd;

    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_short_body, sizeof(k_golden_bad_short_body), &rd),
               "parser_rejects_short_body");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_comma2, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_bad_comma");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_status, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_bad_status");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_grossnet, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_bad_grossnet");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_device_id, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_device_id_over_99");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_unit, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_bad_unit");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_weight_char, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_bad_weight_char");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(
                   k_golden_bad_weight_empty, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_empty_weight");

    /* NULL handling is part of the contract too. */
    test_check(!cas_ci2001_protocol.parse_frame_with_length(NULL, GOLDEN_BODY_SIZE, &rd),
               "parser_rejects_null_frame");
    test_check(!cas_ci2001_protocol.parse_frame_with_length(k_golden_bad_status,
                                                           GOLDEN_BODY_SIZE, NULL),
               "parser_rejects_null_out");
}

/* ---- 3. Framing replay through the transport abstraction --------------
 * Drives the real scale_reader task over a mock transport and asserts the
 * externally observable event stream. This is what proves the refactor moved
 * the UART without moving behaviour. */

#define REPLAY_EVENT_QUEUE_LEN 32U

typedef struct {
    mock_transport_t mock;
    QueueHandle_t events;
} replay_ctx_t;

static bool replay_start(replay_ctx_t *ctx, uint8_t channel_id)
{
    mock_transport_init(&ctx->mock);

    /* The reader expects an already-open transport, exactly as app_main
     * hands it one after scale_transport_rs232_init(). */
    const scale_link_config_t link_cfg = {
        .uart_port = 0,
        .rx_gpio = -1,
        .tx_gpio = -1,
        .baud_rate = 9600,
    };
    if (scale_transport_open(&ctx->mock.base, &link_cfg) != ESP_OK) return false;

    ctx->events = xQueueCreate(REPLAY_EVENT_QUEUE_LEN, sizeof(scale_event_t));
    if (ctx->events == NULL) return false;

    const scale_reader_config_t cfg = {
        .channel_id = channel_id,
        .transport = &ctx->mock.base,
        .protocols = k_protocols,
        .num_protocols = sizeof(k_protocols) / sizeof(k_protocols[0]),
        .event_queue = ctx->events,
        /* Legacy multi-protocol replay is an explicit test opt-in: the
         * production reader decodes exactly one protocol. */
        .allow_auto_detect = true,
    };
    return scale_reader_start(&cfg) == ESP_OK;
}

/* Collect events for up to timeout_ms. Returns how many landed. */
static size_t replay_collect(replay_ctx_t *ctx, scale_event_t *out,
                             size_t max_events, uint32_t timeout_ms)
{
    size_t count = 0U;
    uint32_t waited = 0U;
    const uint32_t step_ms = 10U;

    while (count < max_events && waited < timeout_ms) {
        scale_event_t ev;
        if (xQueueReceive(ctx->events, &ev, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            out[count++] = ev;
            waited = 0U; /* keep draining while events are flowing */
        } else {
            waited += step_ms;
        }
    }
    return count;
}

static size_t count_readings(const scale_event_t *events, size_t count)
{
    size_t n = 0U;
    for (size_t i = 0U; i < count; i++) {
        if (events[i].type == SCALE_EVENT_READING) n++;
    }
    return n;
}

static size_t count_online(const scale_event_t *events, size_t count)
{
    size_t n = 0U;
    for (size_t i = 0U; i < count; i++) {
        if (events[i].type == SCALE_EVENT_ONLINE) n++;
    }
    return n;
}

static const scale_event_t *first_reading(const scale_event_t *events, size_t count)
{
    for (size_t i = 0U; i < count; i++) {
        if (events[i].type == SCALE_EVENT_READING) return &events[i];
    }
    return NULL;
}

static void test_framing_replay_golden_stream(void)
{
    static replay_ctx_t ctx;
    scale_event_t events[24];
    char prefix[96];

    /* Channel 0 is the first reader task; later replay cases use 1..3. */
    if (!replay_start(&ctx, 0U)) {
        test_check(false, "replay_start_channel0");
        return;
    }
    test_check(true, "replay_start_channel0");

    /*
     * The reader's two-frame ONLINE rule is frozen behaviour: the first
     * valid frame only registers a candidate and emits NOTHING. Two frames
     * are required before ONLINE plus a READING appear.
     */
    const golden_frame_t *first = &k_golden_frames[0];
    (void)mock_transport_push_bytes(&ctx.mock, first->wire, GOLDEN_WIRE_SIZE);

    size_t n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_online(events, n) == 0U, "replay_first_frame_no_online");
    test_check(count_readings(events, n) == 0U, "replay_first_frame_no_reading");

    /* Second identical frame confirms the link and yields the reading. */
    (void)mock_transport_push_bytes(&ctx.mock, first->wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);

    test_check(count_online(events, n) == 1U, "replay_second_frame_one_online");
    test_check(count_readings(events, n) == 1U, "replay_second_frame_one_reading");

    const scale_event_t *rd_ev = first_reading(events, n);
    test_check(rd_ev != NULL, "replay_second_frame_reading_present");
    if (rd_ev != NULL) {
        assert_reading_matches_golden(&rd_ev->reading, first, "replay_first");
        test_check(rd_ev->reading.channel_id == 0U, "replay_first_channel_id");
    }

    /* Once online, every subsequent golden frame emits exactly one READING
     * carrying the frozen parser output. Push each remaining frame once. */
    for (size_t i = 1U; i < GOLDEN_FRAME_COUNT; i++) {
        const golden_frame_t *g = &k_golden_frames[i];
        (void)mock_transport_push_bytes(&ctx.mock, g->wire, GOLDEN_WIRE_SIZE);

        n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
        test_check(count_online(events, n) == 0U, "replay_no_redundant_online");

        if (g->expect_overload) {
            /* OL (overload) is NOT a weight: the reader must emit no weight
             * reading for it (only a status-only overload event). */
            bool weight_reading = false;
            bool overload_event = false;
            for (size_t k = 0U; k < n; k++) {
                if (events[k].type != SCALE_EVENT_READING) continue;
                if (events[k].reading.has_display || events[k].reading.has_gross) weight_reading = true;
                if (events[k].reading.overload) overload_event = true;
            }
            test_check(!weight_reading, "replay_overload_frame_never_a_weight");
            test_check(overload_event, "replay_overload_frame_reported_status_only");
            continue;
        }
        test_check(count_readings(events, n) == 1U, "replay_one_reading_per_frame");

        rd_ev = first_reading(events, n);
        (void)snprintf(prefix, sizeof(prefix), "replay_%s", g->name);
        test_check(rd_ev != NULL, "replay_frame_reading_present");
        if (rd_ev != NULL) {
            assert_reading_matches_golden(&rd_ev->reading, g, prefix);
        }
    }

    /*
     * Device ID 10 is 0x0A, i.e. LF. It reached the parser intact above
     * (golden index 4). Re-check explicitly so a framing regression that
     * splits the record on that byte cannot hide in the loop.
     */
    const golden_frame_t *lf = &k_golden_frames[4];
    test_check(lf->wire[6] == 0x0A, "replay_lf_frame_has_embedded_lf");
    (void)mock_transport_push_bytes(&ctx.mock, lf->wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 1U, "replay_lf_collision_one_reading");
    rd_ev = first_reading(events, n);
    if (rd_ev != NULL) {
        assert_reading_matches_golden(&rd_ev->reading, lf, "replay_lf_collision");
        test_check(rd_ev->reading.has_display && nearly_equal(rd_ev->reading.display, 10.0),
                   "replay_lf_collision_weight_intact");
    }

    /* A malformed frame must produce no READING and must not drop the link. */
    const uint8_t bad_wire[GOLDEN_WIRE_SIZE] = {
        'X','X', ',', 'G','S', ',', 0x01, 0x00, ',',
        '0','0','1','0','.','0','0','0', 0x00, 'k','g',
        '\r', '\n'
    };
    (void)mock_transport_push_bytes(&ctx.mock, bad_wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 0U, "replay_malformed_no_reading");

    /* And the link recovers: the next good frame is still accepted. */
    (void)mock_transport_push_bytes(&ctx.mock, first->wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 1U, "replay_recovered_after_malformed");
}

/* ---- 4. Transport fault events reach the reader's diagnostics ----------
 * The abstraction must carry link faults, not just bytes. A FIFO overflow
 * must flush and drop the partial frame without ever emitting a weight. */
static void test_transport_fault_events(void)
{
    static replay_ctx_t ctx;
    scale_event_t events[16];

    if (!replay_start(&ctx, 1U)) {
        test_check(false, "replay_start_channel1");
        return;
    }
    test_check(true, "replay_start_channel1");

    /* Get the link ONLINE first so a subsequent reading is observable. */
    const golden_frame_t *g = &k_golden_frames[0];
    (void)mock_transport_push_bytes(&ctx.mock, g->wire, GOLDEN_WIRE_SIZE);
    (void)mock_transport_push_bytes(&ctx.mock, g->wire, GOLDEN_WIRE_SIZE);
    (void)replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);

    /*
     * FIFO overflow with a partial frame in flight.
     *
     * The frozen reader drains first and then counts whatever is left in the
     * frame buffer as dropped. That means the partial is NOT emitted as a
     * weight (it is unparseable) and the counters move; it does not mean a
     * good frame queued alongside the overflow survives untouched. Asserting
     * the latter would be asserting behaviour this code never had.
     *
     * What must hold: the overflow is counted, the partial produces no
     * weight, and the link recovers on the next complete frame.
     */
    const uint8_t partial[4] = { 'S','T',',','G' };
    (void)mock_transport_push_bytes(&ctx.mock, partial, sizeof(partial));
    (void)mock_transport_push_event(&ctx.mock, SCALE_XPORT_EV_FIFO_OVF);

    size_t n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 0U,
               "fault_overflow_emits_no_weight");

    scale_reader_diagnostics_t diag;
    if (scale_reader_get_diagnostics(1U, &diag)) {
        test_check(diag.fifo_overflows >= 1U, "fault_overflow_counted");
        test_check(diag.dropped_bytes > 0U, "fault_dropped_bytes_counted");
    } else {
        test_check(false, "fault_diagnostics_available");
    }

    /* Recovery: after a flush the next bytes are not known to start a frame,
     * so the reader resyncs to the next terminator. The first frame after the
     * overflow is sacrificed (fail-safe) and the following one is accepted. */
    (void)mock_transport_push_bytes(&ctx.mock, g->wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 0U, "fault_overflow_resyncs_to_terminator");
    (void)mock_transport_push_bytes(&ctx.mock, g->wire, GOLDEN_WIRE_SIZE);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 400U);
    test_check(count_readings(events, n) == 1U, "fault_overflow_recovers_next_frame");

    const scale_event_t *rd_ev = first_reading(events, n);
    test_check(rd_ev != NULL, "fault_recovery_reading_present");
    if (rd_ev != NULL) {
        assert_reading_matches_golden(&rd_ev->reading, g, "fault_recovery");
    }

    if (scale_reader_get_diagnostics(1U, &diag)) {
        test_check(diag.valid_frames >= 3U, "fault_valid_frames_counted");
    }

    /* Framing errors are counted, and must not produce a weight. */
    (void)mock_transport_push_event(&ctx.mock, SCALE_XPORT_EV_FRAME_ERR);
    (void)mock_transport_push_event(&ctx.mock, SCALE_XPORT_EV_BREAK);
    n = replay_collect(&ctx, events, sizeof(events) / sizeof(events[0]), 300U);
    test_check(count_readings(events, n) == 0U, "fault_line_errors_emit_no_weight");

    if (scale_reader_get_diagnostics(1U, &diag)) {
        test_check(diag.framing_errors >= 1U, "fault_framing_error_counted");
        test_check(diag.break_events >= 1U, "fault_break_counted");
    }
}

/* ---- 5. Transport contract: inert when misconfigured -------------------
 * A NULL transport must not crash and must never manufacture data. */
static void test_transport_null_safety(void)
{
    uint8_t buf[8];
    scale_xport_event_t ev;
    size_t len = 123U;

    test_check(scale_transport_read(NULL, buf, sizeof(buf), 0U) == 0,
               "xport_null_read_is_zero");
    test_check(!scale_transport_wait_event(NULL, &ev, 0U), "xport_null_wait_is_false");
    test_check(scale_transport_pending_events(NULL) == 0U, "xport_null_pending_is_zero");
    test_check(scale_transport_get_buffered_len(NULL, &len) != ESP_OK,
               "xport_null_buffered_fails");
    test_check(len == 0U, "xport_null_buffered_clears_out");
    test_check(strcmp(scale_transport_name(NULL), "none") == 0, "xport_null_name_is_none");

    /* A transport with ops but no open must also refuse to read. */
    mock_transport_t mock;
    mock_transport_init(&mock);
    test_check(scale_transport_read(&mock.base, buf, sizeof(buf), 0U) == 0,
               "xport_unopened_read_is_zero");
    test_check(!scale_transport_wait_event(&mock.base, &ev, 0U),
               "xport_unopened_wait_is_false");
}

void test_transport_golden_run(void)
{
    test_golden_parser_outputs();
    test_golden_parser_rejects_malformed();
    test_framing_replay_golden_stream();
    test_transport_fault_events();
    test_transport_null_safety();
}
