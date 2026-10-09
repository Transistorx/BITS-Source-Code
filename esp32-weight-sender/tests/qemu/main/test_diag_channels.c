/*
 * Per-channel diagnostics, CH1 selection and cross-channel isolation.
 *
 * Covers the 2026-10-09 field report (CI-150A on RS-485 CH1, health line
 * showing rx=0 valid=0 bad=0 while genuine weights flowed):
 *   - counters are per channel and independent
 *   - rejected frames are counted by reason; their sum equals invalid_frames
 *   - LF bytes inside the raw CI-150A body (device ID, lamp octet, pad) no
 *     longer split a frame
 *   - every link transition records its reason and the counters at that instant
 *   - the selected channel (CH1) alone drives weight, freshness and link state;
 *     a fresh reading on the other channel never makes the link look healthy
 */

#include "test_diag_channels.h"
#include "test_reader_safety.h"

#include "test_harness.h"

#include "board_pins.h"
#include "cas_ci2001_parser.h"
#include "log_util.h"
#include "mock_transport.h"
#include "scale_events.h"
#include "scale_manager.h"
#include "scale_reader.h"
#include "weight_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const scale_protocol_t *k_cas_only[] = { &cas_ci2001_protocol };

#define BODY 20U
#define WIRE 22U

/* Build a CR/LF-terminated CI-150A wire frame. */
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

typedef struct {
    mock_transport_t mock;
    QueueHandle_t events;
} rig_t;

static bool rig_start(rig_t *r, uint8_t channel_id)
{
    mock_transport_init(&r->mock);
    const scale_link_config_t link_cfg = {
        .uart_port = 0, .rx_gpio = -1, .tx_gpio = -1, .baud_rate = 9600,
    };
    if (scale_transport_open(&r->mock.base, &link_cfg) != ESP_OK) return false;
    r->events = xQueueCreate(64, sizeof(scale_event_t));
    if (r->events == NULL) return false;
    const scale_reader_config_t cfg = {
        .channel_id = channel_id,
        .transport = &r->mock.base,
        .protocols = k_cas_only,
        .num_protocols = 1U,
        .event_queue = r->events,
    };
    return scale_reader_start(&cfg) == ESP_OK;
}

static void rig_feed(rig_t *r, const uint8_t *data, size_t len)
{
    (void)mock_transport_push_bytes(&r->mock, data, len);
    vTaskDelay(pdMS_TO_TICKS(80));
    scale_event_t ev;
    while (xQueueReceive(r->events, &ev, 0) == pdTRUE) { }
}

static void test_reject_reasons_and_framing(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 3U)) {
        test_check(false, "diag_rig_start");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Lamp octet 0x0A: used to split the frame in two and reject both halves. */
    make_wire(w, "ST", "GS", 1U, 0x0A, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(3U, &d);
    test_check(d.valid_frames == 2U, "diag_lamp_lf_frames_accepted");
    test_check(d.invalid_frames == 0U, "diag_lamp_lf_no_rejects");
    test_check(d.embedded_lf == 2U, "diag_lamp_lf_counted_as_data");
    test_check(d.valid_stable == 2U && d.valid_unstable == 0U, "diag_valid_by_status_stable");
    test_check(d.loop_gap_max_ms < 500U, "diag_reader_loop_not_stalled");
    test_check(d.link_online && d.link_transitions == 1U &&
               d.link_reason == SCALE_LINK_REASON_CONFIRMED,
               "diag_transition_online_confirmed");
    test_check(d.link_trans_valid == 2U && d.link_trans_bytes == 2U * WIRE,
               "diag_transition_records_counters");

    /* Device ID 0x0A AND lamp 0x0A together, and a pad of 0x0A. */
    make_wire(w, "ST", "GS", 0x0A, 0x0A, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    make_wire(w, "US", "GS", 1U, 0x00, "0010.000", 0x0A, "kg");
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(3U, &d);
    test_check(d.valid_frames == 4U && d.invalid_frames == 0U,
               "diag_id_lf_lamp_lf_pad_lf_accepted");
    test_check(d.embedded_lf == 5U, "diag_embedded_lf_total");

    /* Rejected frames by reason (each wire frame is CR/LF terminated). */
    uint8_t bad[WIRE];
    make_wire(bad, "ST", "GS", 1U, 0U, "0010.000", 0U, "kg");
    bad[2] = ';';                                   /* header */
    rig_feed(&rig, bad, WIRE);
    make_wire(bad, "ST", "GS", 100U, 0U, "0010.000", 0U, "kg"); /* id > 99: header */
    rig_feed(&rig, bad, WIRE);
    make_wire(bad, "XX", "GS", 1U, 0U, "0010.000", 0U, "kg");   /* status */
    rig_feed(&rig, bad, WIRE);
    make_wire(bad, "ST", "GS", 1U, 0U, "0010.000", 0U, "zz");   /* field (unit) */
    rig_feed(&rig, bad, WIRE);
    make_wire(bad, "ST", "GS", 1U, 0U, "001X.000", 0U, "kg");   /* field (weight) */
    rig_feed(&rig, bad, WIRE);
    const uint8_t shortf[7] = { 0x01, 0x02, 0x03, 0x04, 0x05, '\r', '\n' }; /* length */
    rig_feed(&rig, shortf, sizeof(shortf));

    scale_reader_get_diagnostics(3U, &d);
    test_check(d.rej_header == 2U, "diag_reject_header_counted");
    test_check(d.rej_status == 1U, "diag_reject_status_counted");
    test_check(d.rej_field == 2U, "diag_reject_field_counted");
    test_check(d.rej_length == 1U, "diag_reject_length_counted");
    test_check(d.invalid_frames == 6U &&
               d.rej_length + d.rej_header + d.rej_status + d.rej_field + d.rej_other ==
                   d.invalid_frames,
               "diag_reject_reasons_sum_to_invalid_frames");
    test_check(d.link_online, "diag_rejects_alone_do_not_drop_link_inside_window");

    /* Last accepted weight is genuine and tracks stable/unstable. */
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(3U, &d);
    test_check(d.has_last_weight && d.last_weight_g == 10000 && d.last_stable,
               "diag_last_weight_stable");
    make_wire(w, "US", "GS", 1U, 0x00, "0005.500", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    scale_reader_get_diagnostics(3U, &d);
    test_check(d.has_last_weight && d.last_weight_g == 5500 && !d.last_stable,
               "diag_last_weight_unstable");
    test_check(d.valid_unstable >= 2U && d.valid_stable >= 3U, "diag_valid_by_status_split");

    /* Silence: 3 s without a valid frame and no RX byte at all. */
    vTaskDelay(pdMS_TO_TICKS(3400));
    scale_reader_get_diagnostics(3U, &d);
    test_check(!d.link_online && d.link_transitions == 2U &&
               d.link_reason == SCALE_LINK_REASON_TIMEOUT_NO_BYTES,
               "diag_transition_offline_silent_line");
    test_check(d.link_trans_valid == d.valid_frames,
               "diag_offline_transition_records_counters");
}

static void test_offline_with_rejected_bytes(void)
{
    static rig_t rig;
    scale_reader_diagnostics_t d;
    uint8_t w[WIRE];

    if (!rig_start(&rig, 4U)) {
        test_check(false, "diag_rig2_start");
        return;
    }
    make_wire(w, "ST", "GS", 1U, 0x00, "0010.000", 0x00, "kg");
    rig_feed(&rig, w, WIRE);
    rig_feed(&rig, w, WIRE);

    /* Bytes keep arriving but none is an acceptable frame. */
    uint8_t bad[WIRE];
    make_wire(bad, "XX", "GS", 1U, 0U, "0010.000", 0U, "kg");
    for (int i = 0; i < 17; i++) {
        rig_feed(&rig, bad, WIRE);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    scale_reader_get_diagnostics(4U, &d);
    test_check(!d.link_online && d.link_reason == SCALE_LINK_REASON_TIMEOUT_REJECTED,
               "diag_transition_offline_bytes_rejected");
    test_check(d.rej_status >= 10U, "diag_rejected_stream_counted");
    test_check(strcmp(scale_link_reason_name(SCALE_LINK_REASON_TIMEOUT_NO_BYTES),
                      scale_link_reason_name(SCALE_LINK_REASON_TIMEOUT_REJECTED)) != 0,
               "diag_reason_names_distinct");

    /* Independence: channel 3 (previous test) is unaffected by channel 4 traffic. */
    scale_reader_diagnostics_t other;
    scale_reader_get_diagnostics(3U, &other);
    test_check(other.bytes_received != d.bytes_received, "diag_counters_are_per_channel");
}

static void send_reading(QueueHandle_t q, uint8_t channel_id, double kg, uint32_t ts_ms)
{
    scale_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.channel_id = channel_id;
    ev.type = SCALE_EVENT_READING;
    ev.reading = (scale_reading_t){
        .valid = true, .has_display = true, .display = kg, .display_is_physical = true,
        .has_gross = true, .gross = kg, .gross_is_physical = true,
        .has_unit = true, .has_status = true, .stable = true,
        .timestamp_ms = ts_ms, .channel_id = channel_id,
    };
    strcpy(ev.reading.unit, "kg");
    (void)xQueueSend(q, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
}

static void test_selected_channel_isolation(QueueHandle_t manager)
{
    weight_sample_t s;

    weight_source_start(); /* link back to WAITING, sequence reset */

    /* Selection CH1: a fresh frame on CH2 must be invisible. */
    weight_source_select_channel(1U);
    test_check(weight_source_selected_channel() == 1U, "sel_channel_1_selected");
    send_reading(manager, 1U, 7.0, weight_source_now_ms());      /* CH2 fresh */
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_NONE,
               "sel_other_channel_fresh_frame_ignored");
    weight_source_log_cas_status(weight_source_now_ms());
    test_check(weight_source_cas_link_state() == CAS_LINK_WAITING,
               "sel_other_channel_cannot_make_link_online");
    test_check(!weight_source_have_valid_cas() && weight_source_cas_sequence() == 0U,
               "sel_other_channel_leaves_sequence_and_stamp_untouched");
    int32_t g;
    test_check(!weight_source_last_weight_g(&g), "sel_other_channel_gives_no_health_weight");

    /* The selected channel is the only one that counts. */
    send_reading(manager, 0U, 3.25, weight_source_now_ms());     /* CH1 fresh */
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL,
               "sel_selected_channel_frame_accepted");
    test_check(s.channel == 1U && s.weight_g == 3250 && s.stable,
               "sel_sample_is_ch1_3250g");
    test_check(!s.channel2_valid && s.channel1_valid,
               "sel_bundle_contains_only_selected_channel");
    weight_source_log_cas_status(weight_source_now_ms());
    test_check(weight_source_cas_link_state() == CAS_LINK_ONLINE, "sel_link_online_from_ch1");

    /* CH1 goes stale (3.5 s old) while CH2 stays fresh: must NOT stay healthy. */
    send_reading(manager, 0U, 3.25, weight_source_now_ms() - 3500U);
    send_reading(manager, 1U, 9.0, weight_source_now_ms());
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_NONE,
               "sel_stale_ch1_not_rescued_by_fresh_ch2");

    /* Selecting CH2 explicitly switches the source (explicit, never automatic). */
    weight_source_select_channel(2U);
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.channel == 2U &&
               s.weight_g == 9000,
               "sel_explicit_ch2_selection_follows_ch2");

    /* Invalid selection value means "any" only for host tests. */
    weight_source_select_channel(7U);
    test_check(weight_source_selected_channel() == 0U, "sel_invalid_value_maps_to_any");

    weight_source_select_channel(0U); /* leave the default for later suites */
}

static void test_board_selection(void)
{
    board_channel_t original = board_pinmap_active_channel();
    test_check(board_pinmap_active_channel() == board_pinmap_kconfig_channel(),
               "board_active_defaults_to_kconfig");
    test_check(board_pinmap_set_active_channel(BOARD_CH_SCALE_1) &&
               board_pinmap_active_channel() == BOARD_CH_SCALE_1,
               "board_select_ch1");
    const board_scale_link_t *l = board_pinmap_get(board_pinmap_active_channel());
    test_check(l != NULL && l->uart_port == 1 && l->tx_gpio == 13 && l->rx_gpio == 15,
               "board_ch1_is_uart1_tx13_rx15");
    test_check(!board_pinmap_set_active_channel((board_channel_t)5) &&
               board_pinmap_active_channel() == BOARD_CH_SCALE_1,
               "board_invalid_channel_rejected_selection_unchanged");
    test_check(board_pinmap_set_active_channel(BOARD_CH_SCALE_2) &&
               board_pinmap_active_channel() == BOARD_CH_SCALE_2,
               "board_select_ch2");
    (void)board_pinmap_set_active_channel(original);
}

static void test_formatters(void)
{
    char b[LOG_CHAN_SUMMARY_MAX];
    log_chan_t c = {
        .ch = 1, .uart = 1, .selected = true, .bytes = 1234, .bytes_per_s = 96,
        .frames = 56, .valid = 50, .valid_st = 40, .valid_us = 10, .rej_len = 1, .rej_hdr = 2, .rej_stat = 3,
        .rej_field = 4, .rej_other = 5, .rej_term = 6, .uart_faults = 7, .loop_gap_ms = 41,
        .has_weight = true, .weight_g = 660, .stable = true, .have_age = true,
        .age_ms = 120, .link = "ONLINE", .have_seq = true, .cas_seq = 50,
    };
    test_check(log_fmt_chan_summary(b, sizeof(b), &c) > 0U, "fmt_chan_summary_ok");
    test_check(strstr(b, "RX CH1/UART1 sel=Y") != NULL && strstr(b, "bytes=1234 (96B/s)") != NULL &&
               strstr(b, "valid=50(st=40 us=10)") != NULL &&
               strstr(b, "rej[len=1 hdr=2 stat=3 field=4 other=5 term=6]") != NULL &&
               strstr(b, "faults=7 loopgap=41ms") != NULL && strstr(b, "last=0.660kg ST") != NULL &&
               strstr(b, "age=120ms") != NULL && strstr(b, "link=ONLINE") != NULL &&
               strstr(b, "cas_seq=50") != NULL,
               "fmt_chan_summary_fields");

    memset(&c, 0, sizeof(c));
    c.ch = 2; c.uart = 2; c.link = "OFFLINE";
    test_check(log_fmt_chan_summary(b, sizeof(b), &c) > 0U &&
               strstr(b, "RX CH2/UART2 sel=N bytes=0 (0B/s)") != NULL &&
               strstr(b, "loopgap=0ms last=n/a age=n/a") != NULL && strstr(b, "cas_seq=n/a") != NULL,
               "fmt_chan_summary_silent_channel_is_na_not_zero");
    test_check(log_fmt_chan_summary(b, 30U, &c) == 0U, "fmt_chan_summary_truncation_reported");
    test_check(log_fmt_chan_summary(NULL, 0U, &c) == 0U && log_fmt_chan_summary(b, sizeof(b), NULL) == 0U,
               "fmt_chan_summary_null_safe");

    log_link_transition_t t = {
        .ch = 1, .uart = 1, .selected = true, .to_online = false,
        .reason = "no-valid-frame-3s,no-rx-bytes", .bytes = 99, .valid = 7, .rejected = 2,
    };
    test_check(log_fmt_link_transition(b, sizeof(b), &t) > 0U &&
               strstr(b, "CH1/UART1 link ONLINE -> OFFLINE reason=no-valid-frame-3s,no-rx-bytes") != NULL &&
               strstr(b, "bytes=99 valid=7 rejected=2 sel=Y") != NULL,
               "fmt_link_transition_offline");
    t.to_online = true;
    test_check(log_fmt_link_transition(b, sizeof(b), &t) > 0U &&
               strstr(b, "link OFFLINE -> ONLINE") != NULL,
               "fmt_link_transition_online");
    test_check(log_fmt_link_transition(NULL, 0U, &t) == 0U, "fmt_link_transition_null_safe");
}

static void test_classifier(void)
{
    uint8_t w[WIRE];
    make_wire(w, "ST", "GS", 1U, 0U, "0010.000", 0U, "kg");
    test_check(cas_ci2001_protocol.classify_reject(w, BODY) == SCALE_REJ_NONE, "cls_valid_none");
    test_check(cas_ci2001_protocol.classify_reject(w, 19U) == SCALE_REJ_LENGTH, "cls_length");
    w[3] = 'Z';
    test_check(cas_ci2001_protocol.classify_reject(w, BODY) == SCALE_REJ_HEADER, "cls_header");
    make_wire(w, "XX", "GS", 1U, 0U, "0010.000", 0U, "kg");
    test_check(cas_ci2001_protocol.classify_reject(w, BODY) == SCALE_REJ_STATUS, "cls_status");
    make_wire(w, "ST", "GS", 1U, 0U, "0010.000", 0U, "zz");
    test_check(cas_ci2001_protocol.classify_reject(w, BODY) == SCALE_REJ_FIELD, "cls_unit_field");
    test_check(cas_ci2001_protocol.classify_reject(NULL, BODY) == SCALE_REJ_OTHER, "cls_null_other");
}

void test_diag_channels_run(QueueHandle_t manager_queue)
{
    test_classifier();
    test_board_selection();
    test_formatters();
    test_reject_reasons_and_framing();
    test_offline_with_rejected_bytes();
    if (manager_queue != NULL) test_selected_channel_isolation(manager_queue);
    test_reader_safety_run(manager_queue);
}
