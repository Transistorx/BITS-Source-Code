#include "weight_receiver.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "relay_driver.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "RELAY_CTRL";

/*
 * Purpose-built scanner for this protocol's message shape.
 *
 * IDF 6.1 ships no JSON library (cJSON was removed from the core and there is
 * no espressif/cJSON component), and the message is a flat object of scalars,
 * so a general parser would be a dependency bought for three fields.
 *
 * The bias is fail-closed: anything not recognised is rejected, and nested
 * objects or arrays are rejected rather than parsed, because this protocol
 * never sends them. A rejected message cannot move the timestamp or the relay,
 * so strictness here is the safe direction.
 */

typedef struct {
    bool   type_seen;
    bool   type_is_weight;
    bool   weight_seen;      /* weight_kg: convenience field, fraction allowed */
    double weight_value;
    bool   grams_seen;       /* weight_g: authoritative integer grams */
    double grams_value;
    bool   weight1_seen, weight1_null;
    double weight1_value;
    bool   weight2_seen, weight2_null;
    double weight2_value;
    bool   sequence_seen;
    double sequence_value;
    bool age_seen;
    double age_value;
    bool   source_seen;
    char   source[8];
    bool   simulated_seen;
    bool   simulated_value;
    bool   stable_seen;
    bool   stable_value;
    bool   stable1_seen, stable1_value;
    bool   stable2_seen, stable2_value;
    bool   cycle_seen;
    double cycle_value;
    bool   scale_id_seen, boot_id_seen;
    char   scale_id[17];
    char   boot_id[9];
} weight_fields_t;

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Copies at most cap-1 bytes; a longer string is truncated but still valid. */
static bool scan_string(const char **pp, const char *end, char *out, size_t cap)
{
    const char *p = *pp;
    if (p >= end || *p != '"') return false;
    p++;

    size_t n = 0U;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            if (p + 1 >= end) return false;
            if (n + 1U < cap) out[n++] = p[1];
            p += 2;
            continue;
        }
        if ((unsigned char)*p < 0x20U) return false;
        if (n + 1U < cap) out[n++] = *p;
        p++;
    }
    if (p >= end) return false;

    out[n] = '\0';
    *pp = p + 1;
    return true;
}

static bool scan_number(const char **pp, const char *end, double *out)
{
    const char *start = *pp;
    const char *p = start;

    if (p < end && (*p == '-' || *p == '+')) p++;

    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') { p++; digits = true; }
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') { p++; digits = true; }
    }
    if (!digits) return false;

    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '-' || *p == '+')) p++;
        bool exp_digits = false;
        while (p < end && *p >= '0' && *p <= '9') { p++; exp_digits = true; }
        if (!exp_digits) return false;
    }

    char buf[64];
    size_t n = (size_t)(p - start);
    if (n == 0U || n >= sizeof(buf)) return false;
    memcpy(buf, start, n);
    buf[n] = '\0';

    char *parse_end = NULL;
    double value = strtod(buf, &parse_end);
    if (parse_end == NULL || *parse_end != '\0') return false;

    *out = value;
    *pp = p;
    return true;
}

/* Skips one scalar value. Composites are rejected, not skipped. */
static bool skip_scalar_value(const char **pp, const char *end)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end) return false;

    if (*p == '"') return scan_string(&p, end, (char[1]){0}, 1U) ? (*pp = p, true) : false;
    if (*p == 't') { if (end - p >= 4 && memcmp(p, "true", 4) == 0)  { *pp = p + 4; return true; } return false; }
    if (*p == 'f') { if (end - p >= 5 && memcmp(p, "false", 5) == 0) { *pp = p + 5; return true; } return false; }
    if (*p == 'n') { if (end - p >= 4 && memcmp(p, "null", 4) == 0)  { *pp = p + 4; return true; } return false; }

    if (*p == '-' || *p == '+' || (*p >= '0' && *p <= '9')) {
        double ignored;
        return scan_number(&p, end, &ignored) ? (*pp = p, true) : false;
    }

    return false;
}

static bool scan_object(const char *json, size_t len, weight_fields_t *fields)
{
    const char *p = json;
    const char *end = json + len;

    memset(fields, 0, sizeof(*fields));

    p = skip_ws(p, end);
    if (p >= end || *p != '{') return false;
    p++;

    p = skip_ws(p, end);
    if (p < end && *p == '}') {
        p = skip_ws(p + 1, end);
        return p == end;
    }

    for (;;) {
        char key[32];
        p = skip_ws(p, end);
        if (!scan_string(&p, end, key, sizeof(key))) return false;

        p = skip_ws(p, end);
        if (p >= end || *p != ':') return false;
        p = skip_ws(p + 1, end);

        if (strcmp(key, "type") == 0) {
            char value[24];
            if (!scan_string(&p, end, value, sizeof(value))) return false;
            fields->type_seen = true;
            fields->type_is_weight = (strcmp(value, "weight") == 0);
        } else if (strcmp(key, "weight_kg") == 0) {
            if (!scan_number(&p, end, &fields->weight_value)) return false;
            fields->weight_seen = true;
        } else if (strcmp(key, "weight_g") == 0) {
            if (!scan_number(&p, end, &fields->grams_value)) return false;
            fields->grams_seen = true;
        } else if (strcmp(key, "weight1_g") == 0 || strcmp(key, "weight2_g") == 0) {
            bool first = (key[6] == '1');
            bool *seen = first ? &fields->weight1_seen : &fields->weight2_seen;
            bool *is_null = first ? &fields->weight1_null : &fields->weight2_null;
            double *value = first ? &fields->weight1_value : &fields->weight2_value;
            if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
                *is_null = true;
                p += 4;
            } else if (!scan_number(&p, end, value)) {
                return false;
            }
            *seen = true;
        } else if (strcmp(key, "sequence") == 0) {
            if (!scan_number(&p, end, &fields->sequence_value)) return false;
            fields->sequence_seen = true;
        } else if (strcmp(key, "age_ms") == 0) {
            if (!scan_number(&p, end, &fields->age_value)) return false;
            fields->age_seen = true;
        } else if (strcmp(key, "source") == 0) {
            /* Captured for the log only; the relay rule ignores it. */
            if (!scan_string(&p, end, fields->source, sizeof(fields->source))) return false;
            fields->source_seen = true;
        } else if (strcmp(key, "simulated") == 0) {
            if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
                fields->simulated_value = true;
                p += 4;
            } else if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
                fields->simulated_value = false;
                p += 5;
            } else {
                return false;   /* fail closed on a non-boolean */
            }
            fields->simulated_seen = true;
        } else if (strcmp(key, "stable") == 0) {
            if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
                fields->stable_value = true;
                p += 4;
            } else if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
                fields->stable_value = false;
                p += 5;
            } else {
                return false;   /* fail closed on a non-boolean */
            }
            fields->stable_seen = true;
        } else if (strcmp(key, "stable1") == 0 || strcmp(key, "stable2") == 0) {
            bool first = (key[6] == '1');
            bool *seen = first ? &fields->stable1_seen : &fields->stable2_seen;
            bool *value = first ? &fields->stable1_value : &fields->stable2_value;
            if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
                *value = true;
                p += 4;
            } else if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
                *value = false;
                p += 5;
            } else {
                return false;
            }
            *seen = true;
        } else if (strcmp(key, "scale_id") == 0) {
            if (!scan_string(&p, end, fields->scale_id, sizeof(fields->scale_id))) return false;
            fields->scale_id_seen = true;
        } else if (strcmp(key, "boot_id") == 0) {
            if (!scan_string(&p, end, fields->boot_id, sizeof(fields->boot_id))) return false;
            fields->boot_id_seen = true;
        } else if (strcmp(key, "simulation_cycle") == 0) {
            if (!scan_number(&p, end, &fields->cycle_value)) return false;
            fields->cycle_seen = true;
        } else if (!skip_scalar_value(&p, end)) {
            return false;
        }

        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return false;
    }

    p = skip_ws(p, end);
    return p == end;
}

static _Atomic uint32_t s_last_valid_ms;
static atomic_bool s_have_valid;
static _Atomic uint32_t s_last_sequence;
static atomic_bool s_have_sequence;
static _Atomic weight_link_state_t s_link_state = WEIGHT_LINK_UNKNOWN;

/* Latest accepted message, republished for other tasks (the web API reads it
 * while the WebSocket task writes it). The struct copy is guarded by a
 * critical section so a reader can never observe a half-updated weight. */
static portMUX_TYPE s_latest_lock = portMUX_INITIALIZER_UNLOCKED;
static weight_msg_t s_latest;
static uint32_t s_latest_ms;

/* Policy hook; NULL = the built-in legacy group behaviour. */
static weight_actuation_fn s_actuation;
/* Explicit group state: the last commanded ON/OFF state of all 12 relays.
 * Commands and transition logs are issued only when the desired state flips,
 * so holding >= trigger produces no repeated writes and no log flood. */
static atomic_bool s_group_on;
static _Atomic uint8_t s_manual_relay_channel; /* 0=none, 1=CH1, 2=CH2 */

/* Rejection log rate limit: a peer streaming malformed frames (or an old
 * sender transmitting out-of-window values) must not flood the console. */
#define REJECT_LOG_INTERVAL_MS 30000U
static uint32_t s_rejected_count;
static uint32_t s_last_reject_log_ms;

static void set_link_state(weight_link_state_t state)
{
    if (s_link_state == state) return;
    s_link_state = state;

    switch (state) {
    case WEIGHT_LINK_OK:
        ESP_LOGI(TAG, "Weight link OK");
        break;
    case WEIGHT_LINK_TIMEOUT:
        ESP_LOGW(TAG, "Weight data timeout");
        break;
    case WEIGHT_LINK_LOST:
        ESP_LOGW(TAG, "Weight link lost - relay forced OFF");
        break;
    default:
        break;
    }
}

weight_msg_result_t weight_msg_parse(const char *json, size_t len, weight_msg_t *out)
{
    if (json == NULL || out == NULL || len == 0U) return WEIGHT_MSG_REJECTED;

    weight_fields_t fields;
    if (!scan_object(json, len, &fields)) return WEIGHT_MSG_REJECTED;
    if (!fields.type_seen || !fields.type_is_weight) return WEIGHT_MSG_REJECTED;
    if (!fields.grams_seen && !fields.weight_seen) return WEIGHT_MSG_REJECTED;

    /* Integer grams are authoritative: the threshold decision must never ride
     * on a rounded kg field (9.5 kg rounds to 10 and would falsely trigger).
     * weight_g must be integral; a kg-only message may carry a fraction and is
     * converted by rounding. Range is checked before any cast, so out-of-window
     * or non-finite values never reach the relay. */
    int32_t grams;
    if (fields.grams_seen) {
        double g = fields.grams_value;
        if (!isfinite(g) || g != floor(g)) return WEIGHT_MSG_REJECTED;
        if (g < (double)WEIGHT_G_MIN || g > (double)WEIGHT_G_MAX) return WEIGHT_MSG_REJECTED;
        grams = (int32_t)g;
    } else {
        double kg = fields.weight_value;
        if (!isfinite(kg)) return WEIGHT_MSG_REJECTED;
        double g = kg * 1000.0;
        if (g < ((double)WEIGHT_G_MIN - 0.5) || g > ((double)WEIGHT_G_MAX + 0.5)) {
            return WEIGHT_MSG_REJECTED;
        }
        grams = (int32_t)lround(g);
        if (grams < WEIGHT_G_MIN || grams > WEIGHT_G_MAX) return WEIGHT_MSG_REJECTED;
    }

    memset(out, 0, sizeof(*out));
    out->weight_g = grams;
    out->weight_kg = (int)lround((double)grams / 1000.0);

    if (fields.sequence_seen &&
        isfinite(fields.sequence_value) &&
        fields.sequence_value >= 0.0 &&
        fields.sequence_value <= 4294967295.0) {
        out->sequence = (uint32_t)fields.sequence_value;
        out->has_sequence = true;
    }
    if (fields.age_seen) {
        if (!isfinite(fields.age_value) || fields.age_value < 0 ||
            fields.age_value > WEIGHT_MESSAGE_TIMEOUT_MS ||
            floor(fields.age_value) != fields.age_value) return WEIGHT_MSG_REJECTED;
        out->age_ms = (uint32_t)fields.age_value;
    }

    if (fields.source_seen) {
        out->has_source = true;
        memcpy(out->source, fields.source, sizeof(out->source));
    }

    /* Optional simulator fields. A non-integer or out-of-range cycle is
     * dropped rather than rejected: the weight itself is what the rule needs,
     * and a peer should not be able to make a valid reading unusable by
     * mangling a diagnostic field. */
    /* Production rejects simulated samples outright. A simulated frame must
     * never drive a dispense: it is not a physical measurement. */
    out->simulated = fields.simulated_seen && fields.simulated_value;
    if (out->simulated) {
        return WEIGHT_MSG_REJECTED;
    }
    /* Also reject anything labelled as coming from a simulator source. */
    if (fields.source_seen) {
        if (strcmp(fields.source, "SIM") == 0 ||
            strcmp(fields.source, "sim") == 0) {
            return WEIGHT_MSG_REJECTED;
        }
    }
    out->stable = fields.stable_seen && fields.stable_value;
    out->has_stable = fields.stable_seen;
    out->has_channel_fields = fields.weight1_seen || fields.weight2_seen;
    if (fields.weight1_seen && !fields.weight1_null &&
        isfinite(fields.weight1_value) && fields.weight1_value >= 0.0 &&
        fields.weight1_value <= (double)WEIGHT_G_MAX &&
        fields.weight1_value == floor(fields.weight1_value)) {
        out->has_weight1 = true;
        out->weight1_g = (int32_t)fields.weight1_value;
    }
    if (fields.weight2_seen && !fields.weight2_null &&
        isfinite(fields.weight2_value) && fields.weight2_value >= 0.0 &&
        fields.weight2_value <= (double)WEIGHT_G_MAX &&
        fields.weight2_value == floor(fields.weight2_value)) {
        out->has_weight2 = true;
        out->weight2_g = (int32_t)fields.weight2_value;
    }
    out->has_stable1 = fields.stable1_seen;
    out->stable1 = fields.stable1_seen && fields.stable1_value;
    out->has_stable2 = fields.stable2_seen;
    out->stable2 = fields.stable2_seen && fields.stable2_value;
    if (fields.scale_id_seen) {
        out->has_scale_id = true;
        memcpy(out->scale_id, fields.scale_id, sizeof(out->scale_id));
    }
    if (fields.boot_id_seen) {
        out->has_boot_id = true;
        memcpy(out->boot_id, fields.boot_id, sizeof(out->boot_id));
    }
    if (fields.cycle_seen && isfinite(fields.cycle_value) &&
        fields.cycle_value >= 0.0 && fields.cycle_value <= 4294967295.0) {
        out->simulation_cycle = (uint32_t)fields.cycle_value;
        out->has_cycle = true;
    }

    return WEIGHT_MSG_ACCEPTED;
}

bool weight_should_energize_g(int32_t weight_g)
{
    /* Integer comparison against the centralized trigger — no float equality. */
    return weight_g >= (int32_t)WEIGHT_TRIGGER_G;
}

uint32_t weight_sequence_gap(uint32_t last, uint32_t current)
{
    /* A wrap or a repeat is not a gap; only a forward jump is. */
    if (current <= last) return 0U;
    return (current - last) - 1U;
}

bool weight_link_timed_out(uint32_t now_ms, uint32_t last_valid_ms, uint32_t timeout_ms)
{
    return (now_ms - last_valid_ms) > timeout_ms;
}

esp_err_t weight_receiver_init(void)
{
    s_last_valid_ms = 0U;
    s_have_valid = false;
    s_last_sequence = 0U;
    s_have_sequence = false;
    s_link_state = WEIGHT_LINK_UNKNOWN;
    s_group_on = false;
    s_rejected_count = 0U;
    s_last_reject_log_ms = 0U;
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest_ms = 0U;
    return ESP_OK;
}

void weight_receiver_on_message(const char *json, size_t len, uint32_t now_ms)
{
    /* Bounded so a corrupt or oversized frame cannot flood the monitor. */
    const int log_len = (len > 200U) ? 200 : (int)len;

    weight_msg_t msg;
    if (weight_msg_parse(json, len, &msg) != WEIGHT_MSG_ACCEPTED) {
        /* Rejected input never moves the timestamp and never touches the relay.
         * Logged at most once per REJECT_LOG_INTERVAL_MS, with the running
         * total so the silence is still countable. */
        s_rejected_count++;
        if (s_rejected_count == 1U ||
            (now_ms - s_last_reject_log_ms) >= REJECT_LOG_INTERVAL_MS) {
            ESP_LOGW(TAG, "invalid message rejected (%lu total): %.*s",
                     (unsigned long)s_rejected_count, log_len, json);
            s_last_reject_log_ms = now_ms;
        }
        return;
    }

    /* Every accepted weight prints; the raw JSON stays at debug level. */
    ESP_LOGD(TAG, "RX weight = %.2f kg | source=%s",
             (double)msg.weight_g / 1000.0,
             msg.has_source ? msg.source : "unknown");
    ESP_LOGD(TAG, "RX %.*s", log_len, json);

    if (msg.has_sequence) {
        if (s_have_sequence) {
            uint32_t delta = msg.sequence - s_last_sequence;
            /* Production dispense feed is ordered within a WS session.
             * Retransmitted/older data cannot renew control freshness. The
             * legacy LEVEL_GROUP bench policy retains its previous semantics. */
            if (s_actuation && (delta == 0U || delta >= 0x80000000U)) return;
            uint32_t missed = weight_sequence_gap(s_last_sequence, msg.sequence);
            if (missed > 0U) {
                ESP_LOGW(TAG, "missed %u message(s) before sequence %u",
                         (unsigned)missed, (unsigned)msg.sequence);
            }
        }
        s_last_sequence = msg.sequence;
        s_have_sequence = true;
    }

    set_link_state(WEIGHT_LINK_OK);

    /* Republish for the other tasks that observe the feed (web API, the
     * dispense controller's freshness view). Guarded because the writers and
     * the readers are different tasks and a torn read would surface as a
     * nonsense weight on the UI. */
    portENTER_CRITICAL(&s_latest_lock);
    s_last_valid_ms = now_ms - msg.age_ms;
    s_have_valid = true;
    s_latest = msg;
    s_latest_ms = s_last_valid_ms;
    portEXIT_CRITICAL(&s_latest_lock);

    /* Policy split. Everything above — validation, the timestamp, the link
     * state, the latest value — is common to both modes and is what the
     * failsafe relies on. Only the valve DECISION differs. */
    if (s_actuation != NULL) {
        s_actuation(&msg, s_last_valid_ms);
        return;
    }

    /* Legacy level/group mode. All 12 relays act as one logical group:
     * >= trigger ON, below OFF.
     * Commanded (and logged) only when the group state flips; the driver's
     * per-channel cache makes even a redundant command cheap. */
    bool energize = weight_should_energize_g(msg.weight_g);

    /* FIELD DIAGNOSTIC, now at debug level. This ran at INFO once per accepted
     * message during the relay-actuation investigation, at the then-mandated
     * rate. That investigation is closed — the hardware is confirmed (PCF8574T
     * at 0x22, Relay 1 = P6, Relay 2 = P7) — so the per-message line is no
     * longer evidence anyone needs at INFO, and leaving it there would be the
     * per-loop spam the logging requirements forbid. The transition lines
     * below still fire at INFO, which is where the state change actually is. */
    ESP_LOGD(TAG, "threshold check | weight=%.2f kg | trigger=%.2f kg | desired=%s",
             (double)msg.weight_g / 1000.0,
             (double)WEIGHT_TRIGGER_G / 1000.0,
             energize ? "ON" : "OFF");

    if (energize != s_group_on) {
        esp_err_t err;
        if (energize) {
            ESP_LOGI(TAG, "RX weight = %.2f kg | threshold reached | ALL RELAYS -> ON",
                     (double)msg.weight_g / 1000.0);
            err = relay_all_on();
            /* Per-channel command-vs-effect dump, OFF->ON transition only.
             * Same reasoning as the line above: demoted to DEBUG now that the
             * actuation question is closed. The wording follows the ACTIVE
             * backend — on the I2C expander no GPIO is driven, so cmd =
             * cached commanded state and ack = whether the last expander
             * write for that channel ACKed; on the GPIO fallback it reports
             * the pad level (which reads 0 under QEMU, where GPIO output
             * readback is not emulated — expected, and asserted nowhere). */
            for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
                /* The driver owns the wording: where a channel's output
                 * actually lands is a PCF8574 detail, and this module must not
                 * know one. */
                char where[48];
                (void)relay_describe_channel(i, where, sizeof(where));
                ESP_LOGD(TAG, "Relay%u -> %s (cmd=%d pad=%d err=%s)",
                         (unsigned)(i + 1U), where,
                         (int)relay_get_state(i),
                         (int)relay_pad_is_energized(i),
                         esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "RX weight = %.2f kg | ALL RELAYS -> OFF",
                     (double)msg.weight_g / 1000.0);
            err = relay_all_off();
            /* TEMPORARY FIELD DIAGNOSTIC: aggregated OFF confirmation.
             * CONDITIONAL on the driver result: this line used to print
             * unconditionally, so a suppressed or failed OFF command still
             * announced "OFF" while the hardware stayed energized (field
             * incident: polarity inversion on the I2C expander backend).
             * The log must never claim a state the driver did not confirm. */
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "ALL RELAYS -> OFF (driver confirmed)");
            } else {
                ESP_LOGE(TAG, "ALL RELAYS -> OFF COMMAND FAILED: %s - "
                              "relay state NOT confirmed OFF",
                         esp_err_to_name(err));
            }
        }
        s_group_on = energize;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "group command failed: %s", esp_err_to_name(err));
        }
    }
}

void weight_receiver_on_link_lost(uint32_t now_ms)
{
    (void)now_ms;
    s_have_valid = false;
    s_have_sequence = false; /* new socket session may be a rebooted sender */
    s_group_on = false;
    s_manual_relay_channel = 0; /* Wi-Fi loss is always fatal — no exemption */
    (void)relay_all_off();
    set_link_state(WEIGHT_LINK_LOST);
}

void weight_receiver_set_manual_channel(uint8_t channel)
{
    s_manual_relay_channel = (channel <= 2U) ? channel : 0U;
}

uint8_t weight_receiver_manual_channel(void)
{
    return s_manual_relay_channel;
}

/* Stale-weight failsafe that spares the manually-controlled relay. Wi-Fi loss
 * and E-Stop still call relay_all_off() unconditionally — those are never
 * exempted. */
static void failsafe_off_except_manual(void)
{
    if (s_manual_relay_channel == 0U) {
        (void)relay_all_off();
        return;
    }
    uint8_t spare = (uint8_t)(s_manual_relay_channel - 1U);
    for (uint8_t i = 0; i < 2U; ++i) {
        if (i != spare) (void)relay_set(i, false);
    }
}

void weight_receiver_tick(uint32_t now_ms)
{
    if (!s_have_valid) {
        /* Enforce the failsafe even while no valid message is held. on_message
         * (WebSocket task) and on_link_lost (Wi-Fi event task) are not mutually
         * serialised: a disconnect landing between on_message's
         * s_have_valid=true and its group command would otherwise leave all 12
         * relays energized with s_have_valid already cleared, and nothing
         * would ever turn them off. relay_all_off() is idempotent
         * (cache-checked, silent), so re-asserting it every tick heals that
         * race within one supervisor period. */
        s_group_on = false;
        failsafe_off_except_manual();
        return;
    }
    if (!weight_link_timed_out(now_ms, s_last_valid_ms, WEIGHT_MESSAGE_TIMEOUT_MS)) return;

    s_have_valid = false;
    s_group_on = false;
    failsafe_off_except_manual();
    set_link_state(WEIGHT_LINK_TIMEOUT);
    ESP_LOGW(TAG, "ALL RELAYS -> OFF (fail-safe)");
}

weight_link_state_t weight_receiver_link_state(void)
{
    return s_link_state;
}

const char *weight_link_state_name(weight_link_state_t state)
{
    switch (state) {
    case WEIGHT_LINK_OK:      return "OK";
    case WEIGHT_LINK_TIMEOUT: return "TIMEOUT";
    case WEIGHT_LINK_LOST:    return "LOST";
    default:                  return "UNKNOWN";
    }
}

void weight_receiver_set_actuation_handler(weight_actuation_fn fn)
{
    s_actuation = fn;
}

bool weight_receiver_get_latest(weight_msg_t *out, uint32_t *out_ms)
{
    if (out == NULL) return false;

    bool have;
    portENTER_CRITICAL(&s_latest_lock);
    have = s_have_valid;
    if (have) {
        *out = s_latest;
        if (out_ms != NULL) *out_ms = s_latest_ms;
    }
    portEXIT_CRITICAL(&s_latest_lock);
    return have;
}

bool weight_receiver_have_valid(void)
{
    return s_have_valid;
}

uint32_t weight_receiver_last_rx_ms(void)
{
    return s_last_valid_ms;
}

uint32_t weight_receiver_last_sequence(void)
{
    return s_last_sequence;
}

bool weight_receiver_have_sequence(void)
{
    return s_have_sequence;
}
