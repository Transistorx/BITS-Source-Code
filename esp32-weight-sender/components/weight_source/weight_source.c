#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "weight_source.h"

#include "log_util.h"
#include "scale_manager.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define LB_TO_G 453.59237

static const char *TAG = "WEIGHT";
static const char *TAG_SCALE = "SCALE";

static uint32_t s_sequence = 1U;

/* CAS provenance tracking. These are advanced ONLY by a frame that was
 * actually received on a UART and successfully parsed by a protocol parser.
 * Nothing here can be written by a simulator, a default or a fallback. */
static _Atomic uint32_t s_cas_sequence;
static _Atomic uint32_t s_last_valid_cas_ms;
static atomic_bool s_have_valid_cas;
static uint32_t s_seen_weight_sequence[WEIGHT_CHANNEL_COUNT];
static _Atomic cas_link_state_t s_cas_link = CAS_LINK_WAITING;
/* Last real weight, for the health line only. Written before s_have_valid_cas
 * is set; read only through weight_source_last_weight_g(), which refuses it
 * once stale. */
static _Atomic int32_t s_last_weight_g;
static log_change_t s_link_change;
/* Overload (OL): the scale is alive but reports no usable weight. */
static atomic_bool s_overload_active;
static _Atomic uint32_t s_overload_events;
static log_change_t s_overload_change;
static _Atomic uint8_t s_selected_channel; /* 0 = any (tests), else 1 or 2 */

void weight_source_select_channel(uint8_t channel)
{
    s_selected_channel = (channel == 1U || channel == 2U) ? channel : 0U;
}

uint8_t weight_source_selected_channel(void)
{
    return s_selected_channel;
}

const char *cas_link_state_name(cas_link_state_t state)
{
    switch (state) {
    case CAS_LINK_WAITING: return "WAITING FOR DATA";
    case CAS_LINK_ONLINE:  return "ONLINE / RECEIVING REAL RS232 DATA";
    case CAS_LINK_STALE:   return "STALE";
    case CAS_LINK_OFFLINE: return "OFFLINE";
    default:               return "UNKNOWN";
    }
}

const char *cas_link_state_short(cas_link_state_t state)
{
    switch (state) {
    case CAS_LINK_WAITING: return "WAITING";
    case CAS_LINK_ONLINE:  return "ONLINE";
    case CAS_LINK_STALE:   return "STALE";
    case CAS_LINK_OFFLINE: return "OFFLINE";
    default:               return "UNKNOWN";
    }
}

bool weight_normalize_g(double value, const char *unit, int *out_grams)
{
    if (unit == NULL || out_grams == NULL) return false;
    if (!isfinite(value)) return false;

    double grams;
    if (strcasecmp(unit, "kg") == 0) {
        grams = value * 1000.0;
    } else if (strcasecmp(unit, "lb") == 0) {
        grams = value * LB_TO_G;
    } else {
        return false;
    }

    if (grams > 2147483647.0 || grams < -2147483648.0) return false;

    *out_grams = (int)lround(grams);
    return true;
}

int weight_grams_to_kg(int grams)
{
    /* Rounded, not truncated, and deliberately not clamped: a reading outside
     * 1..10 is real information the receiver is entitled to reject. */
    return (int)lround((double)grams / 1000.0);
}

bool weight_pick_most_recent(const weight_channel_snapshot_t *channels, size_t count,
                             uint32_t now_ms, uint32_t window_ms,
                             weight_sample_t *out)
{
    if (channels == NULL || out == NULL) return false;

    const weight_channel_snapshot_t *best = NULL;
    for (size_t i = 0U; i < count; i++) {
        if (!channels[i].valid) continue;
        if ((now_ms - channels[i].stamp_ms) > window_ms) continue;

        /* Strictly greater keeps the earliest-scanned winner on a tie, so the
         * lowest channel number wins and the choice is deterministic. */
        if (best == NULL || channels[i].stamp_ms > best->stamp_ms) {
            best = &channels[i];
        }
    }

    if (best == NULL) return false;

    out->weight_g = best->grams;
    out->weight_kg = weight_grams_to_kg(best->grams);
    out->from_scale = true;
    out->channel = best->channel;
    out->stable = best->stable;
    out->source = "CAS_RS232";
    out->stamp_ms = best->stamp_ms;
    out->age_ms = now_ms - best->stamp_ms;
    return true;
}

size_t weight_build_json(char *buf, size_t cap,
                         const weight_sample_t *sample, uint32_t sequence)
{
    if (buf == NULL || sample == NULL || cap == 0U) return 0U;

    /* Production frames are ALWAYS real CAS data. There is no simulated
     * frame shape: a simulator cannot reach this builder. */
    if (!sample->from_scale) return 0U;

    char channel1[16], channel2[16];
    if (sample->channel1_valid)
        (void)snprintf(channel1, sizeof(channel1), "%ld", (long)sample->channel1_weight_g);
    else
        (void)snprintf(channel1, sizeof(channel1), "null");
    if (sample->channel2_valid)
        (void)snprintf(channel2, sizeof(channel2), "%ld", (long)sample->channel2_weight_g);
    else
        (void)snprintf(channel2, sizeof(channel2), "null");

    int written = snprintf(buf, cap,
                           "{\"type\":\"weight\",\"weight_kg\":%d,\"weight_g\":%d,"
                           "\"source\":\"CAS_RS232\",\"channel\":%u,\"stable\":%s,"
                           "\"weight1_g\":%s,\"stable1\":%s,"
                           "\"weight2_g\":%s,\"stable2\":%s,"
                           "\"sequence\":%u,\"cas_seq\":%u,\"age_ms\":%u}",
                           sample->weight_kg, sample->weight_g,
                           (unsigned)sample->channel,
                           sample->stable ? "true" : "false",
                           channel1,
                           sample->channel1_stable ? "true" : "false",
                           channel2,
                           sample->channel2_stable ? "true" : "false",
                           (unsigned)sequence,
                           (unsigned)sample->sequence,
                           (unsigned)sample->age_ms);

    /* snprintf returns the length it would have written, so a short buffer is
     * detected here rather than producing a truncated message on the wire. */
    if (written < 0 || (size_t)written >= cap) return 0U;
    return (size_t)written;
}

uint32_t weight_source_now_ms(void)
{
    /* Must match scale_manager's stamp source. esp_timer is initialised before
     * the scheduler starts, so its epoch differs from the FreeRTOS tick epoch;
     * comparing a tick-based "now" against an esp_timer stamp underflows and
     * makes every fresh reading look stale. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void weight_source_init_logging(void)
{
    /* A CI-150A in stream mode sends about 22 frames/s per channel, so raw
     * frames stay quiet unless CONFIG_SCALE_TRACE_FRAMES is on. */
    log_util_apply_levels();
}

bool weight_sample_from_reading(const scale_reading_t *reading, uint8_t channel,
                                weight_sample_t *out, bool *out_used_display)
{
    if (out_used_display != NULL) *out_used_display = false;
    if (reading == NULL || out == NULL) return false;
    if (!reading->valid || !reading->has_unit) return false;
    /* An overload or error indication is never a weight. */
    if (reading->has_status && (reading->overload || reading->error)) return false;

    double value;
    if (reading->has_display) {
        /* cas_parser marks a register response as not physical; honour that so
         * a raw word cannot be published as a weight. */
        if (!reading->display_is_physical) return false;
        value = reading->display;
        if (out_used_display != NULL) *out_used_display = true;
    } else if (reading->has_gross) {
        /* asuki_parser reports gross and never sets display. */
        if (!reading->gross_is_physical) return false;
        value = reading->gross;
    } else {
        return false;
    }

    int grams = 0;
    if (!weight_normalize_g(value, reading->unit, &grams)) return false;

    out->weight_g = grams;
    out->weight_kg = weight_grams_to_kg(grams);
    out->from_scale = true;
    out->channel = channel;
    /* Stable only if THIS reading carries a status (unknown => not stable). */
    out->stable = reading->has_status && reading->stable;
    out->source = "CAS_RS232";
    return true;
}

bool weight_source_overload_active(void)
{
    return s_overload_active;
}

uint32_t weight_source_overload_events(void)
{
    return s_overload_events;
}

uint32_t weight_source_peek_sequence(void)
{
    return s_sequence;
}

void weight_source_advance_sequence(void)
{
    s_sequence = (s_sequence == UINT32_MAX) ? 1U : s_sequence + 1U;
}

cas_link_state_t weight_source_cas_link_state(void)
{
    return s_cas_link;
}

uint32_t weight_source_last_valid_cas_ms(void)
{
    return s_last_valid_cas_ms;
}

uint32_t weight_source_cas_sequence(void)
{
    return s_cas_sequence;
}

bool weight_source_have_valid_cas(void)
{
    return s_have_valid_cas;
}

bool weight_source_last_weight_g(int32_t *grams)
{
    if (grams == NULL || !s_have_valid_cas) return false;
    /* Same freshness window the publisher uses: a stale weight is not shown. */
    if ((uint32_t)(weight_source_now_ms() - s_last_valid_cas_ms) > WEIGHT_CAS_STALE_MS) {
        return false;
    }
    *grams = s_last_weight_g;
    return true;
}

/* Per-sample provenance is DEBUG only: a streaming scale must not produce an
 * INFO line per sample. The link state line carries the INFO-level story. */
void weight_source_log_cas_sample(const weight_sample_t *sample, uint32_t sequence)
{
    if (sample == NULL || !sample->from_scale) return;
    static uint32_t last_log_ms;
    uint32_t now = weight_source_now_ms();
    if (last_log_ms && now - last_log_ms < WEIGHT_CAS_LOG_PERIOD_MS) return;
    last_log_ms = now;
    char kg[24];
    (void)log_fmt_weight_kg(kg, sizeof(kg), sample->weight_g);
    ESP_LOGD(TAG, "sample weight=%s seq=%u cas_seq=%u age=%ums stable=%s",
             kg, (unsigned)sequence, (unsigned)sample->sequence,
             (unsigned)sample->age_ms, sample->stable ? "YES" : "NO");
}

void weight_source_log_cas_status(uint32_t now_ms)
{
    /* Derive the state from genuine sample history only. ONLINE is reached
     * solely after a valid frame has been parsed — never because a task
     * started. */
    if (!s_have_valid_cas) {
        s_cas_link = CAS_LINK_WAITING;
    } else {
        uint32_t age = now_ms - s_last_valid_cas_ms;
        if (age > WEIGHT_CAS_OFFLINE_MS) s_cas_link = CAS_LINK_OFFLINE;
        else if (age > WEIGHT_CAS_STALE_MS) s_cas_link = CAS_LINK_STALE;
        else s_cas_link = CAS_LINK_ONLINE;
    }

    /* Log ONLY on a genuine state transition. A steady WAITING / STALE /
     * OFFLINE line that repeats the same text every tick is serial noise, not
     * information — the state name is the message, and it has not changed.
     * Per-sample ONLINE provenance is printed by weight_source_log_cas_sample()
     * for each new CAS sequence. */
    /* Overload gets its own distinct transition line. */
    {
        int32_t ovl_prev;
        bool ovl = s_overload_active;
        if (log_on_change(&s_overload_change, ovl ? 1 : 0, &ovl_prev)) {
            if (ovl) {
                ESP_LOGW(TAG_SCALE, "overload: scale reports OL, no weight published "
                                    "(events=%lu)", (unsigned long)s_overload_events);
            } else if (ovl_prev != LOG_CHANGE_NONE) {
                ESP_LOGI(TAG_SCALE, "overload cleared");
            }
        }
    }

    int32_t prev;
    if (!log_on_change(&s_link_change, (int32_t)s_cas_link, &prev)) return;

    /* "A -> B age=3.2s". STALE/OFFLINE mean nothing is published and no
     * fallback weight exists; the relay's stale-weight failsafe takes over. */
    char line[64];
    const char *from = (prev == LOG_CHANGE_NONE)
                           ? "INIT" : cas_link_state_short((cas_link_state_t)prev);
    (void)log_fmt_transition(line, sizeof(line), from, cas_link_state_short(s_cas_link),
                             s_have_valid_cas, now_ms - s_last_valid_cas_ms);

    if (s_cas_link == CAS_LINK_STALE) {
        ESP_LOGW(TAG_SCALE, "%s", line);
    } else if (s_cas_link == CAS_LINK_OFFLINE) {
        ESP_LOGE(TAG_SCALE, "%s", line);
    } else {
        ESP_LOGI(TAG_SCALE, "%s", line);
        if (s_cas_link == CAS_LINK_ONLINE &&
            (prev == CAS_LINK_STALE || prev == CAS_LINK_OFFLINE)) {
            ESP_LOGI(TAG, "weight stream recovered");
        }
    }
}

esp_err_t weight_source_start(void)
{
    s_sequence = 1U;
    s_cas_sequence = 0U;
    s_last_valid_cas_ms = 0U;
    s_have_valid_cas = false;
    memset(s_seen_weight_sequence, 0, sizeof(s_seen_weight_sequence));
    s_cas_link = CAS_LINK_WAITING;
    s_last_weight_g = 0;
    log_change_init(&s_link_change);
    log_change_init(&s_overload_change);
    s_overload_active = false;
    s_overload_events = 0U;
    (void)log_on_change(&s_link_change, (int32_t)CAS_LINK_WAITING, NULL);
    ESP_LOGI(TAG_SCALE, "WAITING (real CAS data only, no fallback weight)");
    return ESP_OK;
}

weight_source_result_t weight_source_get(weight_sample_t *out)
{
    if (out == NULL) return WEIGHT_SOURCE_RESULT_NONE;
    memset(out, 0, sizeof(*out));

    weight_channel_snapshot_t snapshots[WEIGHT_CHANNEL_COUNT];
    uint32_t revisions[WEIGHT_CHANNEL_COUNT] = {0};
    memset(snapshots, 0, sizeof(snapshots));

    bool overload_now = false;
    uint32_t now_for_ovl = weight_source_now_ms();
    for (uint8_t i = 0U; i < WEIGHT_CHANNEL_COUNT; i++) {
        uint8_t selected = s_selected_channel;
        if (selected != 0U && (uint8_t)(i + 1U) != selected) continue;

        scale_channel_status_t status;
        if (!scale_manager_get_channel_status(i, &status)) continue;

        /* OL: nothing is published from this channel while it is overloaded
         * (and only while that indication is itself fresh). */
        if (status.latest_reading.has_status && status.latest_reading.overload) {
            if ((uint32_t)(now_for_ovl - status.status_update_ms) <= WEIGHT_CAS_STALE_MS) {
                overload_now = true;
            }
            continue;
        }

        weight_sample_t candidate;
        bool used_display = false;
        if (!weight_sample_from_reading(&status.latest_reading,
                                        (uint8_t)(i + 1U), &candidate,
                                        &used_display)) {
            continue;
        }

        snapshots[i].valid = true;
        revisions[i] = status.weight_sequence;
        /* Freshness must track the field actually published, not the frame
         * that arrived last: latest_reading merges fields across frames, and
         * last_update_ms advances on ANY reading event. A scale streaming
         * status-only frames (X320-style register traffic) would otherwise
         * keep an arbitrarily old weight "fresh". */
        snapshots[i].stamp_ms = used_display ? status.display_update_ms
                                             : status.gross_update_ms;
        snapshots[i].channel = (uint8_t)(i + 1U);
        snapshots[i].grams = candidate.weight_g;
        /* Stable must come from the SAME frame as the weight value. */
        bool same_frame = status.latest_reading.has_status &&
                          status.status_update_ms == snapshots[i].stamp_ms;
        snapshots[i].stable = same_frame && candidate.stable;
    }

    if (overload_now != (bool)s_overload_active) {
        if (overload_now) s_overload_events++;
        s_overload_active = overload_now;
    }

    uint32_t now = weight_source_now_ms();
    if (!weight_pick_most_recent(snapshots, WEIGHT_CHANNEL_COUNT,
                                 now, WEIGHT_CAS_STALE_MS, out)) {
        /* No fresh real CAS data. Publish NOTHING. There is deliberately no
         * simulator, no default weight, no last-known-as-fresh, and no zero
         * fallback: the relay controller's stale-weight failsafe must fire. */
        return WEIGHT_SOURCE_RESULT_NONE;
    }

    for (uint8_t i = 0U; i < WEIGHT_CHANNEL_COUNT; ++i) {
        if (!snapshots[i].valid ||
            (now - snapshots[i].stamp_ms) > WEIGHT_CAS_STALE_MS)
            continue;
        if (snapshots[i].channel == 1U) {
            out->channel1_valid = true;
            out->channel1_weight_g = snapshots[i].grams;
            out->channel1_stable = snapshots[i].stable;
        } else if (snapshots[i].channel == 2U) {
            out->channel2_valid = true;
            out->channel2_weight_g = snapshots[i].grams;
            out->channel2_stable = snapshots[i].stable;
        }
    }

    /* A genuinely new CAS sample: advance the provenance sequence exactly once
     * per accepted frame bundle and record its timestamp. */
    bool changed = !s_have_valid_cas;
    for (uint8_t i = 0; i < WEIGHT_CHANNEL_COUNT; ++i) {
        if (snapshots[i].valid && revisions[i] != s_seen_weight_sequence[i]) {
            changed = true;
            s_seen_weight_sequence[i] = revisions[i];
        }
    }
    if (changed) s_cas_sequence++;
    s_last_weight_g = out->weight_g;
    s_last_valid_cas_ms = out->stamp_ms;
    s_have_valid_cas = true;
    out->sequence = s_cas_sequence;

    return WEIGHT_SOURCE_RESULT_REAL;
}

/* ---------------------------------------------------------------------------
 * TEST-ONLY simulator helpers (QEMU host tests). Not compiled into production.
 * ------------------------------------------------------------------------ */
#ifdef CONFIG_WEIGHT_DEMO_TEST_SIMULATION

#define WEIGHT_SIM_STEP_COUNT 8U
static const int32_t k_sim_steps_g[WEIGHT_SIM_STEP_COUNT] = {
    8000, 9000, 9500, 10000, 12000, 10500, 9500, 8000
};

int32_t weight_sim_step_g(uint32_t step)
{
    return k_sim_steps_g[step % WEIGHT_SIM_STEP_COUNT];
}

int32_t weight_sim_integrate_g(int32_t current_g, bool coarse, bool fine,
                               uint32_t dt_ms)
{
    int64_t rate_g_s = 0;
    if (coarse) rate_g_s += WEIGHT_SIM_COARSE_RATE_G_S;
    if (fine)   rate_g_s += WEIGHT_SIM_FINE_RATE_G_S;

    int64_t next = (int64_t)current_g;
    if (next < 0) next = 0;

    if (rate_g_s > 0 && dt_ms > 0U) {
        next += rate_g_s * (int64_t)dt_ms / 1000;
    }

    if (next > (int64_t)WEIGHT_SIM_MAX_G) next = WEIGHT_SIM_MAX_G;
    if (next < 0) next = 0;
    return (int32_t)next;
}

void weight_ramp_at(uint32_t elapsed_ms, weight_ramp_sample_t *out)
{
    if (out == NULL) return;

    uint32_t cycle = elapsed_ms / WEIGHT_RAMP_CYCLE_MS;
    uint32_t in_cycle = elapsed_ms % WEIGHT_RAMP_CYCLE_MS;

    out->cycle = cycle;

    if (in_cycle < WEIGHT_RAMP_TOP_MS) {
        uint32_t step = in_cycle / WEIGHT_RAMP_STEP_MS;
        out->weight_g = (int32_t)(step * (uint32_t)WEIGHT_RAMP_STEP_G);
        out->holding = false;
    } else {
        out->weight_g = WEIGHT_RAMP_TOP_G;
        out->holding = true;
    }
}

bool weight_ramp_is_reset(int32_t prev_g, int32_t next_g)
{
    return (prev_g >= WEIGHT_RAMP_TOP_G) && (next_g == 0);
}

#endif /* CONFIG_WEIGHT_DEMO_TEST_SIMULATION */
