#include "weight_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_link_core.h"
#include "weight_receiver.h"

static const char *TAG = "WEIGHT_MQTT";

/* ---- strict flat-object scanner (integers only, no floats, no escapes) ------
 * Same bias as weight_receiver: anything unrecognised is rejected, nested
 * values are rejected, and a rejected message changes no state. */

typedef struct {
    bool sv_seen, boot_seen, seq_seen, up_seen, sid_seen, uart_seen, w_seen, st_seen, age_seen;
    bool ch_seen;                 /* legacy per-channel key: rejects the frame */
    uint32_t sv, seq, up, w, age;
    bool stable;
    char boot[12];
    char sid[20];
    char uart[8];
    bool boot_over, sid_over, uart_over;
} env_t;

static const char *ws_skip(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* No escapes allowed (this protocol never sends them). Truncates into out, flags it. */
static bool scan_str(const char **pp, const char *end, char *out, size_t cap, bool *over)
{
    const char *p = *pp;
    if (p >= end || *p != '"') return false;
    p++;
    size_t n = 0U;
    if (over != NULL) *over = false;
    while (p < end && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20U) return false;
        if (n + 1U < cap) out[n++] = *p; else if (over != NULL) *over = true;
        p++;
    }
    if (p >= end) return false;
    if (cap > 0U) out[n] = '\0';
    *pp = p + 1;
    return true;
}

/* Plain unsigned decimal, 1..10 digits, no sign, fraction or exponent, no leading zero. */
static bool scan_u32(const char **pp, const char *end, uint32_t *out)
{
    const char *p = *pp;
    uint64_t v = 0U;
    size_t n = 0U;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10U + (uint64_t)(*p - '0');
        p++;
        if (++n > 10U) return false;
    }
    if (n == 0U || (n > 1U && *(*pp) == '0') || v > 0xFFFFFFFFULL) return false;
    *out = (uint32_t)v;
    *pp = p;
    return true;
}

static bool scan_bool(const char **pp, const char *end, bool *out)
{
    const char *p = *pp;
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) { *out = true; *pp = p + 4; return true; }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) { *out = false; *pp = p + 5; return true; }
    return false;
}

static bool skip_value(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end) return false;
    if (*p == '"') return scan_str(pp, end, NULL, 0U, NULL);
    bool b;
    if (scan_bool(pp, end, &b)) return true;
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) { *pp = p + 4; return true; }
    size_t n = 0U;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' ||
                       *p == 'e' || *p == 'E')) { p++; n++; }
    if (n == 0U) return false;   /* objects, arrays, NaN, Infinity ... */
    *pp = p;
    return true;
}

/* A known key may appear once; a repeat is ambiguous and rejects the message. */
#define KEY_ONCE(seen) do { if (seen) return false; (seen) = true; } while (0)

static bool scan_env(const char *json, size_t len, env_t *e)
{
    const char *p = json, *end = json + len;
    memset(e, 0, sizeof(*e));
    p = ws_skip(p, end);
    if (p >= end || *p != '{') return false;
    p = ws_skip(p + 1, end);
    if (p < end && *p == '}') return false;   /* empty object: nothing to validate */
    for (;;) {
        char key[24];
        p = ws_skip(p, end);
        if (!scan_str(&p, end, key, sizeof(key), NULL)) return false;
        p = ws_skip(p, end);
        if (p >= end || *p != ':') return false;
        p = ws_skip(p + 1, end);

        if (strcmp(key, "schema_version") == 0) {
            KEY_ONCE(e->sv_seen); if (!scan_u32(&p, end, &e->sv)) return false;
        } else if (strcmp(key, "boot_id") == 0) {
            KEY_ONCE(e->boot_seen); if (!scan_str(&p, end, e->boot, sizeof(e->boot), &e->boot_over)) return false;
        } else if (strcmp(key, "seq") == 0) {
            KEY_ONCE(e->seq_seen); if (!scan_u32(&p, end, &e->seq)) return false;
        } else if (strcmp(key, "uptime_ms") == 0) {
            KEY_ONCE(e->up_seen); if (!scan_u32(&p, end, &e->up)) return false;
        } else if (strcmp(key, "scale_id") == 0) {
            KEY_ONCE(e->sid_seen); if (!scan_str(&p, end, e->sid, sizeof(e->sid), &e->sid_over)) return false;
        } else if (strcmp(key, "src_uart") == 0) {
            KEY_ONCE(e->uart_seen); if (!scan_str(&p, end, e->uart, sizeof(e->uart), &e->uart_over)) return false;
        } else if (strcmp(key, "channel") == 0) {
            e->ch_seen = true;   /* an old per-channel frame: never attributable to a pump */
            if (!skip_value(&p, end)) return false;
        } else if (strcmp(key, "weight_g") == 0) {
            KEY_ONCE(e->w_seen); if (!scan_u32(&p, end, &e->w)) return false;
        } else if (strcmp(key, "stable") == 0) {
            KEY_ONCE(e->st_seen); if (!scan_bool(&p, end, &e->stable)) return false;
        } else if (strcmp(key, "age_ms") == 0) {
            KEY_ONCE(e->age_seen); if (!scan_u32(&p, end, &e->age)) return false;
        } else if (!skip_value(&p, end)) {
            return false;
        }

        p = ws_skip(p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return false;
    }
    return ws_skip(p, end) == end;
}

/* ---- single-scale cache, keyed by (scale_id, boot_id) -------------------- */

typedef struct {
    bool     have;                 /* an ACCEPTED message is cached */
    char     scale[17];            /* always the expected scale_id */
    char     boot[9];
    uint32_t seq, uptime_ms, rx_ms, age_ms;
    int32_t  weight_g;
    bool     stable;
    bool     force_unstable;       /* first message after a boot switch */
    bool     pend;                 /* new boot_id debounce in progress */
    char     pboot[9];
    uint32_t pseq, pup;
} scale_cache_t;

/* Transit-delay guard: minimum of the (local receive - sender uptime_ms) offset
 * over a sliding window of WMQ_BUCKETS x WMQ_BUCKET_MS. No clock sync is claimed;
 * only the CHANGE of the offset matters, so u32 wrap is handled by signed deltas. */
#define WMQ_BUCKETS    8
#define WMQ_BUCKET_MS  5000U
typedef struct {
    bool     any;
    char     boot[9];
    uint32_t start_ms;
    int      idx;
    bool     has[WMQ_BUCKETS];
    uint32_t min[WMQ_BUCKETS];
} guard_t;

static scale_cache_t s_c;
static char s_expected[17] = WEIGHT_MQTT_SCALE_ID_DEFAULT;
static guard_t s_guard;
static bool s_enabled;
static uint32_t s_feed_seq;
static weight_mqtt_stats_t s_stats;
static uint32_t s_last_log_ms;

static void guard_reset(const char *boot)
{
    memset(&s_guard, 0, sizeof(s_guard));
    snprintf(s_guard.boot, sizeof(s_guard.boot), "%s", boot);
}

static void guard_roll(uint32_t now_ms)
{
    if (!s_guard.any) { s_guard.start_ms = now_ms; s_guard.any = true; return; }
    uint32_t el = now_ms - s_guard.start_ms;
    if (el >= (uint32_t)WMQ_BUCKETS * WMQ_BUCKET_MS || (int32_t)el < 0) {
        memset(s_guard.has, 0, sizeof(s_guard.has));
        s_guard.start_ms = now_ms;
        return;
    }
    while (now_ms - s_guard.start_ms >= WMQ_BUCKET_MS) {
        s_guard.idx = (s_guard.idx + 1) % WMQ_BUCKETS;
        s_guard.has[s_guard.idx] = false;
        s_guard.start_ms += WMQ_BUCKET_MS;
    }
}

/* True when `offset` is within the allowed excess of the window minimum. */
static bool guard_check(uint32_t offset, uint32_t now_ms)
{
    guard_roll(now_ms);
    int32_t lowest = 0;   /* most negative delta of any bucket minimum vs offset */
    for (int i = 0; i < WMQ_BUCKETS; i++) {
        if (!s_guard.has[i]) continue;
        int32_t d = (int32_t)(s_guard.min[i] - offset);
        if (d < lowest) lowest = d;
    }
    if (-(int64_t)lowest > (int64_t)WEIGHT_MQTT_TRANSIT_MAX_MS) return false;
    int i = s_guard.idx;
    if (!s_guard.has[i] || (int32_t)(offset - s_guard.min[i]) < 0) {
        s_guard.min[i] = offset;
        s_guard.has[i] = true;
    }
    return true;
}

static bool boot_valid(const char *b, bool over)
{
    if (over || strlen(b) != 8U) return false;
    for (size_t i = 0U; i < 8U; i++) {
        char c = b[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static void note(weight_mqtt_result_t r, uint32_t now_ms)
{
    uint32_t *ctr = NULL;
    switch (r) {
    case WMQ_FED: ctr = &s_stats.fed; break;
    case WMQ_PENDING_BOOT: ctr = &s_stats.pending; break;
    case WMQ_DROP_DISABLED: ctr = &s_stats.dropped_disabled; break;
    case WMQ_DROP_INVALID: ctr = &s_stats.dropped_invalid; break;
    case WMQ_DROP_OLD: ctr = &s_stats.dropped_old; break;
    case WMQ_DROP_TRANSIT: ctr = &s_stats.dropped_transit; break;
    case WMQ_DROP_SCALE: ctr = &s_stats.dropped_scale; break;
    }
    if (ctr != NULL) (*ctr)++;
    if (r >= WMQ_DROP_INVALID && (now_ms - s_last_log_ms) >= 30000U) {
        s_last_log_ms = now_ms;
        ESP_LOGW(TAG, "weight dropped (invalid=%lu old=%lu transit=%lu scale=%lu)",
                 (unsigned long)s_stats.dropped_invalid, (unsigned long)s_stats.dropped_old,
                 (unsigned long)s_stats.dropped_transit, (unsigned long)s_stats.dropped_scale);
    }
}

weight_sel_t weight_mqtt_select(const char *transport, const char *peer_sender_id)
{
    if (transport == NULL || strcmp(transport, "mqtt") != 0) return WEIGHT_SEL_WS;
    return mqtt_link_device_id_valid(peer_sender_id) ? WEIGHT_SEL_MQTT : WEIGHT_SEL_MQTT_BLOCKED;
}

void weight_mqtt_init(void)
{
    memset(&s_c, 0, sizeof(s_c));
    memset(&s_guard, 0, sizeof(s_guard));
    memset(&s_stats, 0, sizeof(s_stats));
    snprintf(s_expected, sizeof(s_expected), "%s", WEIGHT_MQTT_SCALE_ID_DEFAULT);
    s_enabled = false;
    s_feed_seq = 0U;
    s_last_log_ms = 0U;
}

void weight_mqtt_set_enabled(bool enabled) { s_enabled = enabled; }
bool weight_mqtt_enabled(void) { return s_enabled; }
void weight_mqtt_stats(weight_mqtt_stats_t *out) { if (out != NULL) *out = s_stats; }

/* [A-Za-z0-9_-]{1,16}, the same alphabet the sender validates for its scale_id. */
static bool scale_id_valid(const char *id, bool over)
{
    if (id == NULL || over) return false;
    size_t n = strlen(id);
    if (n < 1U || n > 16U) return false;
    for (size_t i = 0U; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '_' || c == '-')) return false;
    }
    return true;
}

bool weight_mqtt_set_expected_scale_id(const char *id)
{
    if (!scale_id_valid(id, false)) {
        s_expected[0] = '\0';   /* nothing can match "": no weight is accepted */
        memset(&s_c, 0, sizeof(s_c));
        return false;
    }
    if (strcmp(s_expected, id) != 0) memset(&s_c, 0, sizeof(s_c));
    snprintf(s_expected, sizeof(s_expected), "%s", id);
    return true;
}

const char *weight_mqtt_expected_scale_id(void) { return s_expected; }

/* Hand the ONE accepted sample to the unchanged acceptance path. The sample is a
 * single-scale reading: weight_g, stable, scale_id, boot_id, a local monotonic
 * sequence and age_ms. There is no channel and no neighbour value.
 *
 * SECURITY: the mqtt transport trusts whoever can publish cas/<peer>/weight/ctl.
 * The broker MUST enforce an ACL so only the paired sender can publish there. */
static void feed_receiver(uint32_t recv_ms)
{
    char out[240];
    int n = snprintf(out, sizeof(out),
        "{\"type\":\"weight\",\"weight_g\":%ld,\"source\":\"MQTT\",\"stable\":%s,"
        "\"scale_id\":\"%s\",\"boot_id\":\"%s\",\"sequence\":%lu,\"age_ms\":%lu}",
        (long)s_c.weight_g, s_c.stable ? "true" : "false", s_c.scale, s_c.boot,
        (unsigned long)(++s_feed_seq), (unsigned long)s_c.age_ms);
    if (n <= 0 || (size_t)n >= sizeof(out)) return;   /* cannot happen; fail closed */
    weight_receiver_on_message(out, (size_t)n, recv_ms);
}

static weight_mqtt_result_t process(const char *json, size_t len, uint32_t recv_ms)
{
    if (!s_enabled) return WMQ_DROP_DISABLED;
    if (json == NULL || len == 0U || len > WEIGHT_MQTT_PAYLOAD_MAX) return WMQ_DROP_INVALID;

    env_t e;
    if (!scan_env(json, len, &e)) return WMQ_DROP_INVALID;
    if (e.sv_seen && e.sv != 1U) return WMQ_DROP_INVALID;
    if (e.ch_seen) return WMQ_DROP_INVALID;        /* no channel field exists any more */
    if (!e.boot_seen || !boot_valid(e.boot, e.boot_over)) return WMQ_DROP_INVALID;
    if (!e.seq_seen || !e.up_seen || !e.sid_seen || !e.w_seen || !e.st_seen || !e.age_seen)
        return WMQ_DROP_INVALID;
    if (!scale_id_valid(e.sid, e.sid_over)) return WMQ_DROP_INVALID;
    if (e.uart_seen && (e.uart_over || (strcmp(e.uart, "UART1") != 0 && strcmp(e.uart, "UART2") != 0)))
        return WMQ_DROP_INVALID;                   /* diagnostic tag only, but it must be well formed */
    if (e.w > (uint32_t)WEIGHT_G_MAX) return WMQ_DROP_INVALID;
    if (e.age > (uint32_t)WEIGHT_MESSAGE_TIMEOUT_MS) return WMQ_DROP_INVALID;
    if (strcmp(e.sid, s_expected) != 0) return WMQ_DROP_SCALE;

    char boot[9];
    for (size_t i = 0U; i < 8U; i++) {
        char c = e.boot[i];
        boot[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    boot[8] = '\0';

    scale_cache_t *c = &s_c;
    bool promoted = false;

    if (c->have && strcmp(c->boot, boot) == 0) {
        /* Same boot: strictly greater seq only (wrap-safe); sender uptime may not run backwards. */
        uint32_t d = e.seq - c->seq;
        if (d == 0U || d >= 0x80000000U) return WMQ_DROP_OLD;
        if ((int32_t)(e.up - c->uptime_ms) < 0) return WMQ_DROP_OLD;
    } else if (c->pend && strcmp(c->pboot, boot) == 0 && e.seq == c->pseq + 1U &&
               (int32_t)(e.up - c->pup) >= 0) {
        /* Second message of a new boot: exactly +1 on the sender's one counter with a
         * monotonic uptime, so duplicates, gaps and reordering never promote. */
        promoted = true;
    } else {
        /* New boot_id (or first message ever): not usable until a 2nd consecutive
         * message arrives. The old cached value stops being used at once. */
        c->have = false;
        c->pend = true;
        snprintf(c->pboot, sizeof(c->pboot), "%s", boot);
        c->pseq = e.seq;
        c->pup = e.up;
        return WMQ_PENDING_BOOT;
    }

    if (promoted && strcmp(s_guard.boot, boot) != 0) guard_reset(boot);
    /* offset = local receive - sender uptime = clock skew + transit delay, so the
     * window minimum is the least-delayed message and the excess is the delay. */
    if (!guard_check(recv_ms - e.up, recv_ms)) return WMQ_DROP_TRANSIT;

    if (promoted) {
        c->pend = false;
        snprintf(c->boot, sizeof(c->boot), "%s", boot);
        c->force_unstable = true;
    }
    memcpy(c->scale, e.sid, sizeof(c->scale) - 1U);   /* validated: 1..16 chars */
    c->scale[sizeof(c->scale) - 1U] = '\0';
    c->have = true;
    c->seq = e.seq;
    c->uptime_ms = e.up;
    c->rx_ms = recv_ms;
    c->age_ms = e.age;
    c->weight_g = (int32_t)e.w;
    c->stable = e.stable && !c->force_unstable;
    c->force_unstable = false;

    feed_receiver(recv_ms);
    return WMQ_FED;
}

weight_mqtt_result_t weight_mqtt_on_payload(const char *json, size_t len, uint32_t recv_ms)
{
    weight_mqtt_result_t r = process(json, len, recv_ms);
    note(r, recv_ms);
    return r;
}
