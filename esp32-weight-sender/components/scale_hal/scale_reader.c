#include "scale_reader.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "log_util.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scale_events.h"
#include "scale_transport.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Production has two channels; the QEMU suite starts many independent readers. */
#ifdef CONFIG_WEIGHT_DEMO_TEST_SIMULATION
#define SCALE_READER_CHANNEL_COUNT 20U
#else
#define SCALE_READER_CHANNEL_COUNT 8U
#endif

/* Legacy multi-protocol auto-detect and the generic ASCII fallback exist only
 * when explicitly selected (QEMU suite / bench). */
#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
#define SCALE_READER_AUTODETECT 1
#else
#define SCALE_READER_AUTODETECT 0
#endif

#ifndef CONFIG_SCALE_READER_STALL_GAP_MS
#define CONFIG_SCALE_READER_STALL_GAP_MS 250
#endif
#define CI2001_PROTOCOL_NAME "CAS_CI150A"
#define CI2001_BODY_SIZE 20U
#define CI2001_LAMP_OFFSET 7U
#define CI2001_WEIGHT_OFFSET 9U
#define CI2001_WEIGHT_SIZE 8U
#define CI2001_PAD_OFFSET 17U
#define RX_CHUNK_SIZE 128U
#define RX_DRAIN_BUDGET 512U
#define RX_RECOVERY_DRAIN_BUDGET 1024U
#define FRAME_BUFFER_SIZE 256U
#define CONNECTION_TIMEOUT_MS 3000U

/* The RX task never logs on error paths: every fault only bumps a counter in
 * scale_reader_diagnostics_t, and the diag task in main reports changes at a
 * bounded rate. Only the opt-in frame trace below logs from this file. */
#if LOG_UTIL_SCALE_TRACE
static const char *TAG = "RS485";
#endif

typedef struct {
    scale_reader_config_t config;
    scale_reader_diagnostics_t diagnostics;
    char frame[FRAME_BUFFER_SIZE];
    size_t frame_length;
    bool discard_until_newline;
    bool discarded_frame_is_invalid;
    bool online;
    bool has_candidate;
    size_t candidate_protocol;
    uint8_t candidate_count;
    bool candidate_is_generic;
    bool candidate_generic_labeled;
    bool candidate_has_unit;
    char candidate_unit[sizeof(((scale_reading_t *)0)->unit)];
    TickType_t candidate_tick;
    TickType_t last_valid_frame_tick;
    uint32_t bytes_since_valid; /* RX bytes seen after the last accepted frame */
    uint32_t overload_since_valid; /* OL frames seen after the last accepted frame */

    /* Protocol lock: once ONLINE only the confirmed parser may produce a frame. */
    bool locked;
    size_t locked_protocol;
    bool locked_is_generic;

    /* Digits after the decimal point of the accepted frames (0xFF = no point;
     * 0xFE = not applicable). Candidate while offline, established once ONLINE:
     * a later frame with a different count is rejected as "format". */
    uint8_t candidate_decimals;
    bool fmt_valid;
    uint8_t fmt_decimals;

    uint8_t raw_tail[SCALE_RAW_TAIL_SIZE];
    size_t raw_tail_len;
} scale_reader_context_t;

static scale_reader_diagnostics_t s_diagnostics[SCALE_READER_CHANNEL_COUNT];
static portMUX_TYPE s_diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t monotonic_time_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void publish_diagnostics(scale_reader_context_t *ctx)
{
    ctx->diagnostics.raw_tail_len = (uint8_t)ctx->raw_tail_len;
    memcpy(ctx->diagnostics.raw_tail, ctx->raw_tail, ctx->raw_tail_len);
    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics[ctx->config.channel_id] = ctx->diagnostics;
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static bool send_scale_event(scale_reader_context_t *ctx, scale_event_type_t type,
                             const scale_reading_t *reading)
{
    scale_event_t event = {
        .channel_id = ctx->config.channel_id,
        .type = type,
    };
    if (reading != NULL) {
        event.reading = *reading;
    }

    if (xQueueSend(ctx->config.event_queue, &event, 0) != pdTRUE) {
        ctx->diagnostics.application_queue_drops++;
        return false;
    }
    uint32_t pending = (uint32_t)uxQueueMessagesWaiting(ctx->config.event_queue);
    if (pending > ctx->diagnostics.application_queue_high_water)
        ctx->diagnostics.application_queue_high_water = pending;
    return true;
}

static bool reading_is_plausible(const scale_reading_t *reading)
{
    if (reading == NULL || !reading->valid ||
        !(reading->has_gross || reading->has_net || reading->has_tare ||
          reading->has_display || reading->has_status)) {
        return false;
    }

    const double max_abs_weight = 1000000000.0;
    if ((reading->has_gross &&
         (!isfinite(reading->gross) || fabs(reading->gross) > max_abs_weight)) ||
        (reading->has_net &&
         (!isfinite(reading->net) || fabs(reading->net) > max_abs_weight)) ||
        (reading->has_tare &&
         (!isfinite(reading->tare) || fabs(reading->tare) > max_abs_weight)) ||
        (reading->has_display &&
         (!isfinite(reading->display) || fabs(reading->display) > max_abs_weight))) {
        return false;
    }
    return true;
}

static bool token_equals_ignore_case(const char *token, size_t token_length,
                                     const char *expected)
{
    size_t expected_length = strlen(expected);
    if (token_length != expected_length) return false;
    for (size_t i = 0; i < token_length; i++) {
        if (tolower((unsigned char)token[i]) !=
            tolower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

#if SCALE_READER_AUTODETECT
static bool is_weight_label(const char *token, size_t token_length)
{
    static const char *const labels[] = {
        "GS", "NT", "WT", "ST", "US", "GROSS", "NET", "WEIGHT", "DISPLAY"
    };

    for (size_t label = 0; label < sizeof(labels) / sizeof(labels[0]); label++) {
        if (token_equals_ignore_case(token, token_length, labels[label])) {
            return true;
        }
    }
    return false;
}

static bool is_weight_unit(const char *token, size_t token_length)
{
    static const char *const units[] = {
        "kg", "g", "mg", "lb", "lbs", "oz", "st", "t", "ton", "tonne"
    };

    for (size_t unit = 0; unit < sizeof(units) / sizeof(units[0]); unit++) {
        if (token_equals_ignore_case(token, token_length, units[unit])) {
            return true;
        }
    }
    return false;
}

/*
 * Conservative fallback for scales that emit an undocumented ASCII line:
 * accept one complete numeric weight token with a unit, or a decimal value
 * under a recognizable weight/display label. The reader requires two
 * consecutive plausible frames before using this fallback to mark a channel
 * online.
 */
static bool parse_generic_ascii_frame(const char *frame, size_t length,
                                      scale_reading_t *reading,
                                      bool *has_label)
{
    if (frame == NULL || reading == NULL || has_label == NULL || length < 2U ||
        length >= FRAME_BUFFER_SIZE) {
        return false;
    }

    memset(reading, 0, sizeof(*reading));
    for (size_t i = 0U; i < length; i++) {
        unsigned char ch = (unsigned char)frame[i];
        if (!(isprint(ch) || ch == '\t')) return false;
    }

    size_t cursor = 0U;
    while (cursor < length && isspace((unsigned char)frame[cursor])) cursor++;

    size_t label_start = cursor;
    while (cursor < length && isalpha((unsigned char)frame[cursor])) cursor++;
    size_t label_length = cursor - label_start;
    *has_label = false;
    if (label_length > 0U) {
        if (!is_weight_label(&frame[label_start], label_length)) return false;
        *has_label = true;
        if (cursor < length && frame[cursor] != ':' &&
            !isspace((unsigned char)frame[cursor])) {
            return false;
        }
        while (cursor < length && isspace((unsigned char)frame[cursor])) cursor++;
        if (cursor < length && frame[cursor] == ':') cursor++;
        while (cursor < length && isspace((unsigned char)frame[cursor])) cursor++;
    }

    if (cursor >= length) return false;
    char *end = NULL;
    float value = strtof(&frame[cursor], &end);
    if (end == &frame[cursor] || !isfinite(value) ||
        fabsf(value) > 1000000000.0f) {
        return false;
    }

    size_t number_end = (size_t)(end - frame);
    if (number_end > length) return false;
    bool has_decimal = memchr(&frame[cursor], '.', number_end - cursor) != NULL;
    cursor = number_end;
    while (cursor < length && isspace((unsigned char)frame[cursor])) cursor++;

    size_t unit_start = cursor;
    while (cursor < length && isalpha((unsigned char)frame[cursor])) cursor++;
    size_t unit_length = cursor - unit_start;
    if (unit_length > 0U) {
        if (!is_weight_unit(&frame[unit_start], unit_length) ||
            unit_length >= sizeof(reading->unit)) {
            return false;
        }
    }
    while (cursor < length && isspace((unsigned char)frame[cursor])) cursor++;
    if (cursor != length || (unit_length == 0U && !has_decimal && !*has_label)) {
        return false;
    }

    reading->valid = true;
    reading->has_display = true;
    reading->display = value;
    if (unit_length > 0U) {
        memcpy(reading->unit, &frame[unit_start], unit_length);
        reading->unit[unit_length] = '\0';
        reading->has_unit = true;
        reading->display_is_physical = true;
    }
    return true;
}

#endif /* SCALE_READER_AUTODETECT */

static bool frame_is_printable_ascii(const char *frame, size_t length)
{
    if (frame == NULL) return false;
    for (size_t i = 0U; i < length; i++) {
        unsigned char byte = (unsigned char)frame[i];
        if (!(isprint(byte) || byte == '\t')) return false;
    }
    return true;
}

static bool parse_scale_frame(scale_reader_context_t *ctx, size_t length,
                              scale_reading_t *reading, size_t *protocol_id,
                              bool *is_generic, bool *generic_labeled)
{
    *is_generic = false;
    *generic_labeled = false;
    bool is_ascii = frame_is_printable_ascii(ctx->frame, length);

    /* Production: exactly protocols[0]. With the legacy auto-detect opt-in the
     * whole list is tried only until the link is confirmed; from then on the
     * confirmed parser is the ONLY one that may produce a frame. */
    size_t first = 0U;
    size_t last = 1U;
#if SCALE_READER_AUTODETECT
    bool try_generic = false;
    if (ctx->config.allow_auto_detect) {
        last = ctx->config.num_protocols;
        try_generic = ctx->config.allow_generic_fallback;
        if (ctx->locked) {
            if (ctx->locked_is_generic) {
                last = 0U;                 /* generic only */
            } else {
                first = ctx->locked_protocol;
                last = first + 1U;
                try_generic = false;
            }
        }
    }
#endif

    for (size_t i = first; i < last; i++) {
        const scale_protocol_t *protocol = ctx->config.protocols[i];
        memset(reading, 0, sizeof(*reading));

        bool parsed = false;
        if (protocol->parse_frame_with_length != NULL) {
            parsed = protocol->parse_frame_with_length(
                (const uint8_t *)ctx->frame, length, reading);
        } else if (is_ascii && protocol->parse_frame != NULL) {
            parsed = protocol->parse_frame(ctx->frame, reading);
        }

        if (parsed &&
            reading_is_plausible(reading)) {
            *protocol_id = i;
            return true;
        }
    }

#if SCALE_READER_AUTODETECT
    if (try_generic && is_ascii &&
        parse_generic_ascii_frame(ctx->frame, length, reading, generic_labeled) &&
        reading_is_plausible(reading)) {
        *protocol_id = ctx->config.num_protocols;
        *is_generic = true;
        return true;
    }
#endif
    return false;
}

static void record_transition(scale_reader_context_t *ctx, bool online,
                              scale_link_reason_t reason, uint64_t now_ms)
{
    scale_reader_diagnostics_t *d = &ctx->diagnostics;
    d->link_online = online;
    d->link_transitions++;
    d->link_reason = reason;
    d->link_transition_ms = now_ms;
    d->link_trans_bytes = d->bytes_received;
    d->link_trans_valid = d->valid_frames;
    d->link_trans_rejected = d->invalid_frames + d->parser_overflows;
}

/* Grams for the diagnostics line only; the production weight path converts in
 * weight_source. Returns false for a unit or value it cannot express. */
static bool reading_grams(const scale_reading_t *r, int32_t *out)
{
    double v;
    if (r == NULL || !r->has_unit) return false;
    if (r->has_display && r->display_is_physical) v = r->display;
    else if (r->has_gross && r->gross_is_physical) v = r->gross;
    else return false;
    double g;
    if (token_equals_ignore_case(r->unit, strlen(r->unit), "kg")) g = v * 1000.0;
    else if (token_equals_ignore_case(r->unit, strlen(r->unit), "lb")) g = v * 453.59237;
    else return false;
    if (!isfinite(g) || fabs(g) > 2147483000.0) return false;
    *out = (int32_t)lround(g);
    return true;
}

#define FORMAT_NOT_APPLICABLE 0xFEU
#define FORMAT_NO_POINT 0xFFU

/* Digits after the decimal point in the CI-150A 8-byte weight field. */
static uint8_t ci2001_weight_decimals(const char *frame)
{
    const char *w = &frame[CI2001_WEIGHT_OFFSET];
    for (size_t i = 0U; i < CI2001_WEIGHT_SIZE; i++) {
        if (w[i] == '.') {
            uint8_t digits = 0U;
            for (size_t j = i + 1U; j < CI2001_WEIGHT_SIZE && w[j] >= '0' && w[j] <= '9'; j++) {
                digits++;
            }
            return digits;
        }
    }
    return FORMAT_NO_POINT;
}

static void mark_valid_frame(scale_reader_context_t *ctx, size_t protocol_id,
                             bool is_generic, bool generic_labeled,
                             scale_reading_t *reading, TickType_t now_tick,
                             uint64_t now_ms, uint8_t decimals)
{
    /* Defence in depth: while ONLINE only the confirmed parser may publish. */
    if (ctx->online && ctx->locked &&
        (ctx->locked_protocol != protocol_id || ctx->locked_is_generic != is_generic)) {
        ctx->diagnostics.invalid_frames++;
        ctx->diagnostics.rej_other++;
        return;
    }

    ctx->diagnostics.valid_frames++;
    ctx->diagnostics.last_valid_frame_timestamp_ms = now_ms;
    ctx->last_valid_frame_tick = now_tick;
    ctx->bytes_since_valid = 0U;
    ctx->overload_since_valid = 0U;
    if (reading->has_status) {
        if (reading->stable) ctx->diagnostics.valid_stable++;
        else ctx->diagnostics.valid_unstable++;
    }
    {
        int32_t grams;
        if (reading_grams(reading, &grams)) {
            ctx->diagnostics.has_last_weight = true;
            ctx->diagnostics.last_weight_g = grams;
            /* Stable only when this very frame carried a status. */
            ctx->diagnostics.last_stable = reading->has_status && reading->stable;
            ctx->diagnostics.last_weight_ms = now_ms;
        }
    }
    reading->channel_id = ctx->config.channel_id;
    reading->timestamp_ms = (uint32_t)now_ms;

    if (!ctx->online) {
        TickType_t confirmation_window = pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS);
        bool generic_signature_matches = !is_generic ||
            (ctx->candidate_is_generic &&
             ctx->candidate_generic_labeled == generic_labeled &&
             ctx->candidate_has_unit == reading->has_unit &&
             (!reading->has_unit ||
              token_equals_ignore_case(ctx->candidate_unit,
                                       strlen(ctx->candidate_unit),
                                       reading->unit)));
        if (!ctx->has_candidate || ctx->candidate_protocol != protocol_id ||
            ctx->candidate_is_generic != is_generic ||
            !generic_signature_matches ||
            ctx->candidate_decimals != decimals ||
            (TickType_t)(now_tick - ctx->candidate_tick) > confirmation_window) {
            ctx->has_candidate = true;
            ctx->candidate_protocol = protocol_id;
            ctx->candidate_is_generic = is_generic;
            ctx->candidate_generic_labeled = generic_labeled;
            ctx->candidate_has_unit = reading->has_unit;
            ctx->candidate_decimals = decimals;
            (void)snprintf(ctx->candidate_unit, sizeof(ctx->candidate_unit), "%s",
                           reading->unit);
            ctx->candidate_count = 1U;
            ctx->candidate_tick = now_tick;
            return;
        }

        if (ctx->candidate_count < UINT8_MAX) ctx->candidate_count++;
        ctx->candidate_tick = now_tick;
        if (ctx->candidate_count < 2U) return;

        ctx->online = true;
        ctx->has_candidate = false;
        ctx->locked = true;
        ctx->locked_protocol = protocol_id;
        ctx->locked_is_generic = is_generic;
        ctx->fmt_valid = true;
        ctx->fmt_decimals = decimals;
        record_transition(ctx, true, SCALE_LINK_REASON_CONFIRMED, now_ms);
        (void)send_scale_event(ctx, SCALE_EVENT_ONLINE, NULL);
    }

    (void)send_scale_event(ctx, SCALE_EVENT_READING, reading);
}

static void count_rejection(scale_reader_context_t *ctx, size_t length)
{
    scale_reject_reason_t reason = SCALE_REJ_OTHER;
    for (size_t i = 0U; i < ctx->config.num_protocols; i++) {
        const scale_protocol_t *protocol = ctx->config.protocols[i];
        if (protocol->classify_reject != NULL) {
            reason = protocol->classify_reject((const uint8_t *)ctx->frame, length);
            break;
        }
    }
    switch (reason) {
    case SCALE_REJ_LENGTH: ctx->diagnostics.rej_length++; break;
    case SCALE_REJ_HEADER: ctx->diagnostics.rej_header++; break;
    case SCALE_REJ_STATUS: ctx->diagnostics.rej_status++; break;
    case SCALE_REJ_FIELD:  ctx->diagnostics.rej_field++; break;
    default:               ctx->diagnostics.rej_other++; break;
    }
}

/* Drop the partial frame and ignore every byte up to the next terminator. Used
 * after ANY event that can have lost or re-timed bytes (FIFO overflow, driver
 * ring overflow, flush, reader stall, assembler overflow): the next bytes are
 * not known to start on a frame boundary, so they must not be parsed. */
static void resync_to_terminator(scale_reader_context_t *ctx)
{
    ctx->diagnostics.dropped_bytes += (uint32_t)ctx->frame_length;
    ctx->frame_length = 0U;
    ctx->discard_until_newline = true;
    ctx->discarded_frame_is_invalid = false;
    ctx->has_candidate = false;
    ctx->candidate_count = 0U;
    ctx->diagnostics.resync_events++;
}

static void reject_parsed(scale_reader_context_t *ctx, uint32_t *counter)
{
    ctx->diagnostics.invalid_frames++;
    (*counter)++;
    ctx->has_candidate = false;
    ctx->candidate_count = 0U;
    ctx->frame_length = 0U;
}

static bool frame_contains_ol(const char *frame, size_t length)
{
    for (size_t i = 0U; i + 1U < length; i++) {
        if (frame[i] == 'O' && frame[i + 1U] == 'L') return true;
    }
    return false;
}

static void finish_frame(scale_reader_context_t *ctx, TickType_t now_tick,
                         uint64_t now_ms)
{
    ctx->diagnostics.frames_received++;

    if (ctx->discard_until_newline) {
        if (ctx->discarded_frame_is_invalid) {
            ctx->diagnostics.invalid_frames++;
            ctx->diagnostics.rej_other++;
        }
        ctx->discard_until_newline = false;
        ctx->discarded_frame_is_invalid = false;
        ctx->frame_length = 0U;
        ctx->has_candidate = false;
        ctx->candidate_count = 0U;
        return;
    }

    while (ctx->frame_length > 0U &&
           (ctx->frame[ctx->frame_length - 1U] == '\r' ||
            ctx->frame[ctx->frame_length - 1U] == ' ' ||
            ctx->frame[ctx->frame_length - 1U] == '\t')) {
        ctx->frame_length--;
    }
    ctx->frame[ctx->frame_length] = '\0';

    /* RAW CAPTURE: the first frame that contains "OL", once per boot. */
    if (!ctx->diagnostics.ol_captured &&
        frame_contains_ol(ctx->frame, ctx->frame_length)) {
        size_t keep = ctx->frame_length < SCALE_OL_RAW_SIZE ? ctx->frame_length
                                                              : SCALE_OL_RAW_SIZE;
        memcpy(ctx->diagnostics.ol_raw, ctx->frame, keep);
        ctx->diagnostics.ol_raw_len = (uint8_t)keep;
        ctx->diagnostics.ol_captured = true;
    }

#if LOG_UTIL_SCALE_TRACE
    /* Opt-in (CONFIG_SCALE_TRACE_FRAMES), DEBUG only: a streaming scale would
     * otherwise bury the monitor. Compiled out by default. */
    ESP_LOGD(TAG, "ch%u frame (%u bytes): %s", (unsigned)ctx->config.channel_id,
             (unsigned)ctx->frame_length, ctx->frame);
#endif

    scale_reading_t reading;
    size_t protocol_id = 0U;
    bool is_generic = false;
    bool generic_labeled = false;
    int64_t parse_started = esp_timer_get_time();
    bool parsed = ctx->frame_length != 0U &&
        parse_scale_frame(ctx, ctx->frame_length, &reading, &protocol_id,
                          &is_generic, &generic_labeled);
    uint32_t parse_us = (uint32_t)(esp_timer_get_time() - parse_started);
    if (!ctx->diagnostics.parser_calls || parse_us < ctx->diagnostics.parser_min_us)
        ctx->diagnostics.parser_min_us = parse_us;
    if (parse_us > ctx->diagnostics.parser_max_us)
        ctx->diagnostics.parser_max_us = parse_us;
    ctx->diagnostics.parser_calls++;
    ctx->diagnostics.parser_total_us += parse_us;
    if (!parsed) {
        ctx->diagnostics.invalid_frames++;
        count_rejection(ctx, ctx->frame_length);
        ctx->has_candidate = false;
        ctx->frame_length = 0U;
        return;
    }

    /* Overload: the scale is alive but the number is not a weight. Never emit
     * it, never refresh the link, never let it confirm the link. While ONLINE
     * a status-only event tells the manager so the previous weight stops being
     * publishable immediately instead of after the freshness window. */
    if (reading.has_status && reading.overload) {
        ctx->diagnostics.overload_frames++;
        ctx->overload_since_valid++;
        ctx->has_candidate = false;
        ctx->candidate_count = 0U;
        ctx->frame_length = 0U;
        if (ctx->online) {
            scale_reading_t ol;
            memset(&ol, 0, sizeof(ol));
            ol.valid = true;
            ol.has_status = true;
            ol.overload = true;
            ol.channel_id = ctx->config.channel_id;
            ol.timestamp_ms = (uint32_t)now_ms;
            (void)send_scale_event(ctx, SCALE_EVENT_READING, &ol);
        }
        return;
    }

    uint8_t decimals = FORMAT_NOT_APPLICABLE;
    const scale_protocol_t *accepted_by =
        is_generic ? NULL : ctx->config.protocols[protocol_id];
    if (accepted_by != NULL && accepted_by->name != NULL &&
        strcmp(accepted_by->name, CI2001_PROTOCOL_NAME) == 0 &&
        ctx->frame_length == CI2001_BODY_SIZE) {
        uint8_t pad = (uint8_t)ctx->frame[CI2001_PAD_OFFSET];
        if (ctx->config.strict_pad && pad != ctx->config.pad_value) {
            reject_parsed(ctx, &ctx->diagnostics.rej_pad);
            return;
        }
        decimals = ci2001_weight_decimals(ctx->frame);
        if (ctx->online && ctx->fmt_valid && decimals != ctx->fmt_decimals) {
            reject_parsed(ctx, &ctx->diagnostics.rej_format);
            return;
        }
        ctx->diagnostics.have_pad_lamp = true;
        ctx->diagnostics.last_pad = pad;
        ctx->diagnostics.last_lamp = (uint8_t)ctx->frame[CI2001_LAMP_OFFSET];
    }

    mark_valid_frame(ctx, protocol_id, is_generic, generic_labeled,
                     &reading, now_tick, now_ms, decimals);
    ctx->frame_length = 0U;
}

/*
 * CI-150A/CI-2001 body bytes 6 (device ID), 7 (lamp status octet) and 17 (pad)
 * are RAW binary, so any of them can be 0x0A (LF). The real terminator is the
 * CR/LF pair after the 20-byte body. Once the fixed ASCII prefix
 * "<US|ST|OL>,<GS|NT>," has been seen, an LF arriving while the body holds
 * exactly 6, 7 or 17 bytes can only be one of those raw bytes and is kept as
 * data. (Previously only the device-ID position was handled, so a lamp octet
 * of 0x0A split every frame into two rejected fragments for as long as the
 * indicator stayed in that state.) A truncated body that already ends in CR
 * at position 17 is still treated as a terminator, so resync costs one frame.
 */
static bool ci2001_lf_is_data(const scale_reader_context_t *ctx)
{
    if (ctx == NULL) return false;
    size_t n = ctx->frame_length;
    if (n != 6U && n != 7U && n != 17U) return false;

    bool known_status =
        (ctx->frame[0U] == 'U' && ctx->frame[1U] == 'S') ||
        (ctx->frame[0U] == 'S' && ctx->frame[1U] == 'T') ||
        (ctx->frame[0U] == 'O' && ctx->frame[1U] == 'L');
    bool known_gross_net =
        (ctx->frame[3U] == 'G' && ctx->frame[4U] == 'S') ||
        (ctx->frame[3U] == 'N' && ctx->frame[4U] == 'T');
    if (!(known_status && ctx->frame[2U] == ',' && known_gross_net &&
          ctx->frame[5U] == ',')) {
        return false;
    }
    if (n == 17U) return ctx->frame[8U] == ',' && ctx->frame[16U] != '\r';
    return true;
}

static void ingest_rx_bytes(scale_reader_context_t *ctx, const uint8_t *data,
                            size_t length)
{
    uint64_t now_ms = monotonic_time_ms();
    TickType_t now_tick = xTaskGetTickCount();
    ctx->diagnostics.bytes_received += (uint32_t)length;
    ctx->diagnostics.last_rx_timestamp_ms = now_ms;

    /* Last raw bytes for the RAW CAPTURE diagnostic. */
    if (length >= SCALE_RAW_TAIL_SIZE) {
        memcpy(ctx->raw_tail, data + (length - SCALE_RAW_TAIL_SIZE), SCALE_RAW_TAIL_SIZE);
        ctx->raw_tail_len = SCALE_RAW_TAIL_SIZE;
    } else {
        size_t total = ctx->raw_tail_len + length;
        if (total > SCALE_RAW_TAIL_SIZE) {
            size_t shift = total - SCALE_RAW_TAIL_SIZE;
            memmove(ctx->raw_tail, ctx->raw_tail + shift, ctx->raw_tail_len - shift);
            ctx->raw_tail_len -= shift;
        }
        memcpy(ctx->raw_tail + ctx->raw_tail_len, data, length);
        ctx->raw_tail_len += length;
    }

    for (size_t i = 0U; i < length; i++) {
        unsigned char byte = data[i];
        ctx->bytes_since_valid++;
        if (byte == '\n') {
            if (ci2001_lf_is_data(ctx)) {
                ctx->diagnostics.embedded_lf++;
            } else {
                finish_frame(ctx, now_tick, now_ms);
                continue;
            }
        }

        if (ctx->discard_until_newline) {
            ctx->diagnostics.dropped_bytes++;
            continue;
        }

        if (ctx->frame_length >= FRAME_BUFFER_SIZE - 1U) {
            ctx->diagnostics.parser_overflows++;
            ctx->diagnostics.dropped_bytes += 1U;
            resync_to_terminator(ctx);
            continue;
        }

        ctx->frame[ctx->frame_length++] = (char)byte;
    }
}

/*
 * Drain bytes from the transport and feed the frame assembler. The only
 * thing that changed versus the RS232-only code is the byte source: the
 * budget, chunking and ingest behaviour are identical, so a silent or
 * erroring link still produces exactly the same diagnostics and still
 * publishes nothing.
 */
static size_t drain_uart(scale_reader_context_t *ctx, size_t byte_budget)
{
    uint8_t buffer[RX_CHUNK_SIZE];
    size_t drained = 0U;

    while (drained < byte_budget) {
        size_t request = byte_budget - drained;
        if (request > sizeof(buffer)) request = sizeof(buffer);
        int length = scale_transport_read(ctx->config.transport, buffer, request, 0U);
        if (length < 0) {
            ctx->diagnostics.uart_read_errors++;
            break;
        }
        if (length == 0) break;
        ingest_rx_bytes(ctx, buffer, (size_t)length);
        drained += (size_t)length;
    }
    return drained;
}

/*
 * React to a link event. The event already arrived through the transport
 * abstraction, so the cases below are physical-layer faults in the generic
 * vocabulary, not ESP-IDF UART constants. Counter names are unchanged
 * because they are part of the frozen diagnostics contract.
 */
static void handle_uart_event(scale_reader_context_t *ctx,
                              const scale_xport_event_t *event)
{
    switch (event->type) {
    case SCALE_XPORT_EV_DATA:
        break;
    case SCALE_XPORT_EV_FIFO_OVF: {
        ctx->diagnostics.fifo_overflows++;
        /* Drain queued bytes first; flush only the exceptional FIFO recovery. */
        (void)drain_uart(ctx, RX_RECOVERY_DRAIN_BUDGET);
        size_t buffered = 0U;
        if (scale_transport_get_buffered_len(ctx->config.transport,
                                             &buffered) == ESP_OK) {
            ctx->diagnostics.dropped_bytes += (uint32_t)buffered;
        }
        (void)scale_transport_flush_input(ctx->config.transport);
        /* Bytes were lost at an unknown position: the next bytes are not known
         * to start a frame (e.g. " 12.345" -> tail "2.345 kg"). */
        resync_to_terminator(ctx);
        break;
    }
    case SCALE_XPORT_EV_BUFFER_FULL:
        ctx->diagnostics.driver_buffer_overflows++;
        /* Retain pending bytes, but the driver dropped bytes after them: the
         * frame in progress is spliced with later data, so resync. */
        (void)drain_uart(ctx, RX_RECOVERY_DRAIN_BUDGET);
        resync_to_terminator(ctx);
        break;
    case SCALE_XPORT_EV_BREAK:
        ctx->diagnostics.break_events++;
        break;
    case SCALE_XPORT_EV_PARITY_ERR:
        ctx->diagnostics.parity_errors++;
        break;
    case SCALE_XPORT_EV_FRAME_ERR:
        ctx->diagnostics.framing_errors++;
        break;
    case SCALE_XPORT_EV_PATTERN_DET:
        ctx->diagnostics.pattern_detections++;
        break;
    default:
        break;
    }
}

/* The reader task did not run for longer than the stall threshold, so bytes
 * waiting in the RX ring were time-stamped late. Fail-safe: drop them and
 * resync to the next terminator. Counted here; the diag task reports it. */
static void handle_reader_stall(scale_reader_context_t *ctx, uint32_t gap_ms)
{
    ctx->diagnostics.reader_stalls++;
    ctx->diagnostics.last_stall_gap_ms = gap_ms;
    size_t buffered = 0U;
    if (scale_transport_get_buffered_len(ctx->config.transport, &buffered) == ESP_OK) {
        ctx->diagnostics.dropped_bytes += (uint32_t)buffered;
    }
    (void)scale_transport_flush_input(ctx->config.transport);
    resync_to_terminator(ctx);
}

static void scale_reader_task(void *arg)
{
    scale_reader_context_t *ctx = (scale_reader_context_t *)arg;
    scale_xport_event_t event;
    const TickType_t offline_timeout = pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS);
    TickType_t last_stack_check = 0;
    TickType_t last_loop_tick = xTaskGetTickCount();
    TickType_t last_drain_tick = last_loop_tick;

    (void)send_scale_event(ctx, SCALE_EVENT_OFFLINE, NULL);

    for (;;) {
        {
            TickType_t loop_tick = xTaskGetTickCount();
            uint32_t gap_ms = (uint32_t)((TickType_t)(loop_tick - last_loop_tick) * portTICK_PERIOD_MS);
            last_loop_tick = loop_tick;
            if (gap_ms > ctx->diagnostics.loop_gap_max_ms) ctx->diagnostics.loop_gap_max_ms = gap_ms;
        }
        if (scale_transport_pending_events(ctx->config.transport) >=
            SCALE_TRANSPORT_EVENT_QUEUE_SIZE) {
            ctx->diagnostics.uart_event_queue_saturations++;
        }

        bool have_event = scale_transport_wait_event(ctx->config.transport, &event, 20U);

        /* Stall check BEFORE anything is read: if this task was not running
         * for longer than the threshold, whatever is waiting in the RX ring was
         * time-stamped late and must not be parsed. Measured from the end of
         * the previous drain, so it also covers a long wait_event(). */
        {
            uint32_t since_drain_ms = (uint32_t)((TickType_t)(xTaskGetTickCount() - last_drain_tick) *
                                                 portTICK_PERIOD_MS);
            uint32_t stall_ms = ctx->config.stall_gap_ms != 0U
                                    ? ctx->config.stall_gap_ms
                                    : (uint32_t)CONFIG_SCALE_READER_STALL_GAP_MS;
            if (since_drain_ms > stall_ms) handle_reader_stall(ctx, since_drain_ms);
        }

        if (have_event) handle_uart_event(ctx, &event);

        (void)drain_uart(ctx, RX_DRAIN_BUDGET);
        TickType_t now = xTaskGetTickCount();
        last_drain_tick = now;

        if (ctx->has_candidate &&
            (TickType_t)(now - ctx->candidate_tick) >= offline_timeout) {
            ctx->has_candidate = false;
            ctx->candidate_count = 0U;
        }

        if (ctx->online &&
            (TickType_t)(now - ctx->last_valid_frame_tick) >= offline_timeout) {
            ctx->online = false;
            ctx->has_candidate = false;
            ctx->candidate_count = 0U;
            ctx->locked = false;
            ctx->fmt_valid = false;
            /* A partial frame left behind means the next bytes are its tail. */
            if (ctx->frame_length != 0U) resync_to_terminator(ctx);
            /* Distinguish "the line went silent", "the scale reports OL" and
             * "bytes arrived but no frame was accepted": different faults. */
            bool rx_after_valid = ctx->bytes_since_valid != 0U;
            scale_link_reason_t why = SCALE_LINK_REASON_TIMEOUT_NO_BYTES;
            if (ctx->overload_since_valid != 0U) why = SCALE_LINK_REASON_TIMEOUT_OVERLOAD;
            else if (rx_after_valid) why = SCALE_LINK_REASON_TIMEOUT_REJECTED;
            record_transition(ctx, false, why, monotonic_time_ms());
            (void)send_scale_event(ctx, SCALE_EVENT_OFFLINE, NULL);
        }

        if (now - last_stack_check >= pdMS_TO_TICKS(10000U)) {
            last_stack_check = now;
            ctx->diagnostics.stack_high_water_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        }
        publish_diagnostics(ctx);
    }
}

esp_err_t scale_reader_start(const scale_reader_config_t *config)
{
    if (config == NULL || config->channel_id >= SCALE_READER_CHANNEL_COUNT ||
        config->transport == NULL || config->transport->ops == NULL ||
        config->protocols == NULL || config->num_protocols == 0U ||
        config->event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Physical-port reservation (console UART, valid peripheral number) is
     * the backend's business and is enforced when the transport is opened.
     * The reader only requires a usable transport handle.
     */
    for (size_t i = 0U; i < config->num_protocols; i++) {
        if (config->protocols[i] == NULL ||
            (config->protocols[i]->parse_frame == NULL &&
             config->protocols[i]->parse_frame_with_length == NULL)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Production decodes exactly ONE protocol: no auto-detect, no generic
     * fallback. The legacy modes exist only in the opt-in configuration. */
    if (config->allow_auto_detect) {
#if !SCALE_READER_AUTODETECT
        return ESP_ERR_INVALID_ARG;
#endif
    } else if (config->num_protocols != 1U || config->allow_generic_fallback) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SCALE_READER_AUTODETECT
    if (config->allow_generic_fallback) return ESP_ERR_INVALID_ARG;
#endif

    scale_reader_context_t *ctx = calloc(1U, sizeof(*ctx));
    if (ctx == NULL) return ESP_ERR_NO_MEM;
    ctx->config = *config;

    portENTER_CRITICAL(&s_diagnostics_lock);
    memset(&s_diagnostics[config->channel_id], 0,
           sizeof(s_diagnostics[config->channel_id]));
    portEXIT_CRITICAL(&s_diagnostics_lock);

    char task_name[configMAX_TASK_NAME_LEN];
    (void)snprintf(task_name, sizeof(task_name), "reader_ch%u",
                   (unsigned)config->channel_id);

    BaseType_t result = xTaskCreate(scale_reader_task, task_name, 4096U,
                                    ctx, 5U, NULL);
    if (result != pdPASS) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *scale_link_reason_name(scale_link_reason_t reason)
{
    switch (reason) {
    case SCALE_LINK_REASON_CONFIRMED:         return "2-valid-frames-in-3s";
    case SCALE_LINK_REASON_TIMEOUT_NO_BYTES:  return "no-valid-frame-3s,no-rx-bytes";
    case SCALE_LINK_REASON_TIMEOUT_REJECTED:  return "no-valid-frame-3s,rx-bytes-rejected";
    case SCALE_LINK_REASON_TIMEOUT_OVERLOAD:  return "no-valid-frame-3s,overload";
    default:                                  return "none";
    }
}

bool scale_reader_get_diagnostics(uint8_t channel_id,
                                  scale_reader_diagnostics_t *out_diagnostics)
{
    if (channel_id >= SCALE_READER_CHANNEL_COUNT || out_diagnostics == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_diagnostics_lock);
    *out_diagnostics = s_diagnostics[channel_id];
    portEXIT_CRITICAL(&s_diagnostics_lock);
    return true;
}
