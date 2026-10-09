#include "scale_cmd.h"

#include <stdio.h>
#include <string.h>

/* ---- Minimal flat-object JSON scan -----------------------------------------
 * Command payloads are one object of scalars. This walks it once, keeps raw
 * slices of the six keys we need and rejects duplicates and trailing garbage.
 * No allocation, no cJSON (it is a managed component on IDF 6.x). */

enum { F_ID, F_TYPE, F_CTYPE, F_CHAN, F_TTL, F_DEV, F_COUNT };
static const char *const k_keys[F_COUNT] = {
    "command_id", "type", "command_type", "channel_id", "ttl_ms", "device_id"
};

typedef struct {
    const char *p;
    size_t n;
    char kind;  /* 's' string (raw, no quotes), 'n' number/literal, 'o' object/array */
} jval_t;

typedef struct {
    bool have[F_COUNT];
    jval_t v[F_COUNT];
} fields_t;

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* p at the opening quote. Returns the char after the closing quote, or NULL. */
static const char *scan_string(const char *p, const char *end, const char **s, size_t *n)
{
    if (p >= end || *p != '"') return NULL;
    const char *q = p + 1;
    while (q < end && *q != '"') {
        if ((unsigned char)*q < 0x20U) return NULL;
        if (*q == '\\') q++;
        q++;
    }
    if (q >= end) return NULL;
    *s = p + 1;
    *n = (size_t)(q - (p + 1));
    return q + 1;
}

static const char *skip_balanced(const char *p, const char *end)
{
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            const char *s;
            size_t n;
            p = scan_string(p, end, &s, &n);
            if (p == NULL) return NULL;
            continue;
        }
        if (*p == '{' || *p == '[') {
            if (++depth > 8) return NULL;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p + 1;
        }
        p++;
    }
    return NULL;
}

static bool parse_fields(const char *payload, size_t len, fields_t *f)
{
    memset(f, 0, sizeof(*f));
    const char *p = payload, *end = payload + len;
    p = skip_ws(p, end);
    if (p >= end || *p != '{') return false;
    p = skip_ws(p + 1, end);
    if (p < end && *p == '}') return skip_ws(p + 1, end) == end;
    for (;;) {
        const char *k;
        size_t kn;
        p = scan_string(skip_ws(p, end), end, &k, &kn);
        if (p == NULL) return false;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return false;
        p = skip_ws(p + 1, end);
        if (p >= end) return false;

        jval_t v;
        if (*p == '"') {
            v.kind = 's';
            p = scan_string(p, end, &v.p, &v.n);
            if (p == NULL) return false;
        } else if (*p == '{' || *p == '[') {
            v.kind = 'o';
            v.p = p;
            const char *after = skip_balanced(p, end);
            if (after == NULL) return false;
            v.n = (size_t)(after - p);
            p = after;
        } else {
            v.kind = 'n';
            v.p = p;
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
                   *p != '\n' && *p != '\r') {
                p++;
            }
            v.n = (size_t)(p - v.p);
            if (v.n == 0U) return false;
        }
        for (int i = 0; i < F_COUNT; i++) {
            if (kn == strlen(k_keys[i]) && memcmp(k, k_keys[i], kn) == 0) {
                if (f->have[i]) return false; /* duplicate key */
                f->have[i] = true;
                f->v[i] = v;
            }
        }
        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == ',') { p++; continue; }
        if (*p == '}') return skip_ws(p + 1, end) == end;
        return false;
    }
}

/* Plain unsigned decimal, no sign, no leading zeros, at most max. */
static bool jval_uint(const jval_t *v, uint32_t max, uint32_t *out)
{
    if (v->kind != 'n' || v->n == 0U || v->n > 10U) return false;
    if (v->n > 1U && v->p[0] == '0') return false;
    uint64_t acc = 0U;
    for (size_t i = 0; i < v->n; i++) {
        if (v->p[i] < '0' || v->p[i] > '9') return false;
        acc = acc * 10U + (uint64_t)(v->p[i] - '0');
    }
    if (acc > max) return false;
    *out = (uint32_t)acc;
    return true;
}

static bool jval_is(const jval_t *v, const char *s)
{
    return v->kind == 's' && v->n == strlen(s) && memcmp(v->p, s, v->n) == 0;
}

/* ---- Ack / dedupe ring ------------------------------------------------------ */

static void ack_fill(scale_cmd_ack_t *a, uint32_t id, uint8_t channel, bool applied,
                     scale_cmd_result_t result, const char *reason)
{
    memset(a, 0, sizeof(*a));
    a->command_id = id;
    a->channel = channel;
    a->applied = applied;
    a->result = result;
    if (reason != NULL) snprintf(a->reason, sizeof(a->reason), "%s", reason);
}

static scale_cmd_slot_t *slot_find(scale_cmd_t *s, uint32_t id)
{
    for (size_t i = 0; i < SCALE_CMD_DEDUPE_SLOTS; i++) {
        if (s->slots[i].stamp != 0U && s->slots[i].id == id) return &s->slots[i];
    }
    return NULL;
}

/* Reuse an empty slot, else evict the oldest FINISHED one. A command that is
 * queued or running is never evicted, so a flood of rejects cannot make an
 * in-flight command look new and run twice. */
static scale_cmd_slot_t *slot_alloc(scale_cmd_t *s)
{
    scale_cmd_slot_t *pick = NULL;
    for (size_t i = 0; i < SCALE_CMD_DEDUPE_SLOTS; i++) {
        scale_cmd_slot_t *c = &s->slots[i];
        if (c->stamp == 0U) { pick = c; break; }
        if (c->done && (pick == NULL || c->stamp < pick->stamp)) pick = c;
    }
    if (pick == NULL) pick = &s->slots[0]; /* unreachable: in-flight <= queue + 1 < slots */
    memset(pick, 0, sizeof(*pick));
    pick->stamp = ++s->next_stamp;
    if (pick->stamp == 0U) pick->stamp = ++s->next_stamp;
    return pick;
}

static scale_submit_t reject(scale_cmd_t *s, scale_cmd_ack_t *ack, uint32_t id,
                             uint8_t channel, scale_cmd_result_t result, const char *reason)
{
    ack_fill(ack, id, channel, false, result, reason);
    scale_cmd_slot_t *slot = slot_alloc(s);
    slot->id = id;
    slot->done = true;
    slot->ack = *ack;
    return SCALE_SUBMIT_ACK;
}

bool scale_cmd_init(scale_cmd_t *s, const scale_cmd_cfg_t *cfg)
{
    if (s == NULL || cfg == NULL || cfg->now_ms == NULL || cfg->link_online == NULL ||
        cfg->post_sample == NULL || cfg->delay_ms == NULL) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (s->cfg.encode == NULL) s->cfg.encode = scale_command_encode;
    s->lock = xSemaphoreCreateMutex();
    s->wake = xSemaphoreCreateCounting(SCALE_CMD_QUEUE_DEPTH, 0);
    return s->lock != NULL && s->wake != NULL;
}

bool scale_cmd_busy(scale_cmd_t *s)
{
    xSemaphoreTake(s->lock, portMAX_DELAY);
    bool busy = s->running;
    xSemaphoreGive(s->lock);
    return busy;
}

scale_submit_t scale_cmd_submit(scale_cmd_t *s, const char *payload, size_t len,
                                bool retain, uint32_t recv_ms, scale_cmd_ack_t *ack)
{
    if (s == NULL || payload == NULL || ack == NULL) return SCALE_SUBMIT_IGNORED;
    /* A retained command is a replay of an old intent: never act, never ACK. */
    if (retain) return SCALE_SUBMIT_IGNORED;

    fields_t f;
    uint32_t id = 0U;
    if (!parse_fields(payload, len, &f) || !f.have[F_ID] ||
        !jval_uint(&f.v[F_ID], UINT32_MAX, &id) || id == 0U) {
        return SCALE_SUBMIT_IGNORED; /* nothing to correlate an ACK with */
    }

    xSemaphoreTake(s->lock, portMAX_DELAY);
    scale_submit_t out;

    scale_cmd_slot_t *dup = slot_find(s, id);
    if (dup != NULL) {
        /* QoS 1 redelivery or HTTP+MQTT double delivery: re-ACK, never re-run. */
        if (dup->done) {
            *ack = dup->ack;
            out = SCALE_SUBMIT_ACK;
        } else {
            out = SCALE_SUBMIT_IGNORED;
        }
        xSemaphoreGive(s->lock);
        return out;
    }

    uint8_t channel = 0U;
    bool channel_null = !f.have[F_CHAN] || (f.v[F_CHAN].kind == 'n' && f.v[F_CHAN].n == 4U &&
                                            memcmp(f.v[F_CHAN].p, "null", 4U) == 0);
    if (!channel_null) {
        if (jval_is(&f.v[F_CHAN], "CH1")) channel = 1U;
        else if (jval_is(&f.v[F_CHAN], "CH2")) channel = 2U;
    }

    scale_cmd_type_t type = SCALE_CMD_ZERO;
    uint32_t ttl = 0U;
    const char *bad = NULL;
    scale_cmd_result_t bad_result = SCALE_RESULT_REJECTED;

    if (f.have[F_TYPE] && jval_is(&f.v[F_TYPE], "ZERO")) type = SCALE_CMD_ZERO;
    else if (f.have[F_TYPE] && jval_is(&f.v[F_TYPE], "TARE")) type = SCALE_CMD_TARE;
    else bad = "unsupported command type (ZERO or TARE only)";

    if (bad == NULL && f.have[F_CTYPE] && !jval_is(&f.v[F_CTYPE], type == SCALE_CMD_ZERO ? "ZERO" : "TARE")) {
        bad = "type and command_type disagree";
    }
    if (bad == NULL && f.have[F_DEV] &&
        !(f.v[F_DEV].kind == 'n' && f.v[F_DEV].n == 4U && memcmp(f.v[F_DEV].p, "null", 4U) == 0) &&
        !jval_is(&f.v[F_DEV], s->cfg.device_id)) {
        bad = "device_id does not match this device";
    }
    if (bad == NULL && channel_null) bad = "channel_id required (CH1 or CH2)";
    if (bad == NULL && channel == 0U) bad = "invalid channel_id";
    if (bad == NULL && channel != s->cfg.active_channel) bad = "channel not served by this device";
    /* ZERO/TARE change what the scale reports, so an undated command with no
     * expiry is refused rather than allowed to linger. */
    if (bad == NULL && (!f.have[F_TTL] || !jval_uint(&f.v[F_TTL], SCALE_CMD_TTL_MAX_MS, &ttl) ||
                        ttl == 0U)) {
        bad = "ttl_ms required (1..60000)";
    }
    if (bad == NULL && ttl > SCALE_CMD_ZT_TTL_MAX_MS) bad = "ttl too long (ttl_ms max 10000 for ZERO/TARE)";
    if (bad == NULL && (uint32_t)(s->cfg.now_ms() - recv_ms) > ttl) {
        bad = "expired before start";
        bad_result = SCALE_RESULT_TIMEOUT;
    }
    if (bad == NULL && s->running) bad = "busy: another scale command is running";
    if (bad == NULL && s->rate_armed[channel - 1U] &&
        (uint32_t)(s->cfg.now_ms() - s->last_accept_ms[channel - 1U]) < SCALE_CMD_ZT_RATE_MS) {
        bad = "rate limited (one ZERO/TARE per 2 s per channel)";
    }

    const char *state_name = "UNKNOWN";
    if (bad == NULL && !s->cfg.link_online(&state_name)) {
        char why[SCALE_CMD_REASON_MAX + 1U];
        snprintf(why, sizeof(why), "scale link not ONLINE (%s)", state_name ? state_name : "UNKNOWN");
        out = reject(s, ack, id, channel, SCALE_RESULT_REJECTED, why);
        xSemaphoreGive(s->lock);
        return out;
    }
    if (bad == NULL && s->q_count >= SCALE_CMD_QUEUE_DEPTH) bad = "command queue full";

    if (bad != NULL) {
        out = reject(s, ack, id, channel, bad_result, bad);
        xSemaphoreGive(s->lock);
        return out;
    }

    scale_cmd_pending_t *p = &s->queue[(s->q_head + s->q_count) % SCALE_CMD_QUEUE_DEPTH];
    p->id = id;
    p->type = type;
    p->channel = channel;
    p->ttl_ms = ttl;
    p->recv_ms = recv_ms;
    s->last_accept_ms[channel - 1U] = s->cfg.now_ms();
    s->rate_armed[channel - 1U] = true;
    s->q_count++;
    scale_cmd_slot_t *slot = slot_alloc(s);
    slot->id = id;
    slot->done = false;
    /* Not an ACK: lets the caller log "TARE requested id=.." after the lock. */
    memset(ack, 0, sizeof(*ack));
    ack->command_id = id;
    ack->channel = channel;
    ack->type = type;
    xSemaphoreGive(s->lock);
    xSemaphoreGive(s->wake);
    return SCALE_SUBMIT_QUEUED;
}

/* ---- Execution (scale_cmd task only) ---------------------------------------- */

#ifdef SCALE_CMD_TEST_HOOKS
/* Test-only (QEMU suite defines SCALE_CMD_TEST_HOOKS; production never does).
 * Default false, so the CONTRACT 8.4 gate holds until a test opts in. */
static bool s_test_post_encode;
void scale_cmd_test_allow_post_encode(bool on) { s_test_post_encode = on; }
static bool test_post_encode_enabled(void) { return s_test_post_encode; }
#else
#define test_post_encode_enabled() (false)
#endif

static void execute(scale_cmd_t *s, const scale_cmd_pending_t *c, scale_cmd_ack_t *ack)
{
    const scale_cmd_cfg_t *cfg = &s->cfg;

    if ((uint32_t)(cfg->now_ms() - c->recv_ms) > c->ttl_ms) {
        ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_TIMEOUT, "expired before start");
        return;
    }
    const char *state_name = "UNKNOWN";
    if (!cfg->link_online(&state_name)) {
        char why[SCALE_CMD_REASON_MAX + 1U];
        snprintf(why, sizeof(why), "scale link not ONLINE (%s)", state_name ? state_name : "UNKNOWN");
        ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_REJECTED, why);
        return;
    }

    uint8_t frame[SCALE_CMD_FRAME_MAX];
    size_t flen = 0U;
    scale_enc_status_t enc = cfg->encode(c->type, c->channel, frame, sizeof(frame), &flen);
    if (!(SCALE_CMD_ENCODER_VERIFIED || test_post_encode_enabled()) || enc != SCALE_ENC_OK) {
        ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_FAILED, SCALE_CMD_REASON_UNVERIFIED);
        return;
    }
    if (cfg->send_frame == NULL || flen == 0U || flen > sizeof(frame)) {
        ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_FAILED, "no scale transmit path");
        return;
    }

    uint32_t base_seq = 0U, seq = 0U;
    int32_t g = 0;
    bool st = false;
    bool have_base = cfg->post_sample(c->channel, &base_seq, &g, &st);
    uint32_t t0 = cfg->now_ms();
    if (!cfg->send_frame(c->channel, frame, flen)) {
        ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_FAILED, "scale transmit failed");
        return;
    }
    while ((uint32_t)(cfg->now_ms() - t0) < cfg->settle_timeout_ms) {
        cfg->delay_ms(20U);
        if (cfg->post_sample(c->channel, &seq, &g, &st) && (!have_base || seq != base_seq)) {
            ack_fill(ack, c->id, c->channel, true, SCALE_RESULT_SUCCESS, NULL);
            ack->has_weight = true;
            ack->weight_g = g;
            ack->stable = st;
            return;
        }
    }
    ack_fill(ack, c->id, c->channel, false, SCALE_RESULT_TIMEOUT,
             "no post-command scale sample before timeout");
}

bool scale_cmd_run_one(scale_cmd_t *s, uint32_t wait_ms, scale_cmd_ack_t *ack)
{
    if (s == NULL || ack == NULL) return false;
    if (xSemaphoreTake(s->wake, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;

    xSemaphoreTake(s->lock, portMAX_DELAY);
    if (s->q_count == 0U) {
        xSemaphoreGive(s->lock);
        return false;
    }
    scale_cmd_pending_t cmd = s->queue[s->q_head];
    s->q_head = (s->q_head + 1U) % SCALE_CMD_QUEUE_DEPTH;
    s->q_count--;
    s->running = true;
    xSemaphoreGive(s->lock);

    execute(s, &cmd, ack);

    xSemaphoreTake(s->lock, portMAX_DELAY);
    scale_cmd_slot_t *slot = slot_find(s, cmd.id);
    if (slot != NULL) {
        slot->ack = *ack;
        slot->done = true;
    }
    s->running = false;
    xSemaphoreGive(s->lock);
    return true;
}

/* ---- ACK JSON -------------------------------------------------------------- */

static const char *result_name(scale_cmd_result_t r)
{
    switch (r) {
    case SCALE_RESULT_ACCEPTED: return "accepted";
    case SCALE_RESULT_SUCCESS:  return "success";
    case SCALE_RESULT_FAILED:   return "failed";
    case SCALE_RESULT_TIMEOUT:  return "timeout";
    default:                    return "rejected";
    }
}

size_t scale_cmd_build_ack_json(const scale_cmd_t *s, const scale_cmd_ack_t *a,
                                char *out, size_t cap)
{
    if (s == NULL || a == NULL || out == NULL || cap == 0U) return 0U;
    /* Reasons are fixed firmware strings (never echoed input), so they need
     * no JSON escaping. */
    size_t n = 0U;
    int w = snprintf(out, cap, "{\"command_id\":%lu,\"state\":\"%s\",\"result\":\"%s\"",
                     (unsigned long)a->command_id, a->applied ? "APPLIED" : "FAILED",
                     result_name(a->result));
    if (w < 0 || (size_t)w >= cap) return 0U;
    n = (size_t)w;
    if (!a->applied) {
        w = snprintf(out + n, cap - n, ",\"error\":\"%s\",\"reason\":\"%s\"", a->reason, a->reason);
        if (w < 0 || (size_t)w >= cap - n) return 0U;
        n += (size_t)w;
    }
    w = snprintf(out + n, cap - n, ",\"device_id\":\"%s\"", s->cfg.device_id);
    if (w < 0 || (size_t)w >= cap - n) return 0U;
    n += (size_t)w;
    if (s->cfg.boot_id[0] != '\0') {
        w = snprintf(out + n, cap - n, ",\"boot_id\":\"%s\"", s->cfg.boot_id);
        if (w < 0 || (size_t)w >= cap - n) return 0U;
        n += (size_t)w;
    }
    if (a->channel == 1U || a->channel == 2U) {
        w = snprintf(out + n, cap - n, ",\"channel_id\":\"CH%u\"", (unsigned)a->channel);
    } else {
        w = snprintf(out + n, cap - n, ",\"channel_id\":null");
    }
    if (w < 0 || (size_t)w >= cap - n) return 0U;
    n += (size_t)w;
    if (a->has_weight) {
        w = snprintf(out + n, cap - n, ",\"weight_g\":%ld,\"stable\":%s",
                     (long)a->weight_g, a->stable ? "true" : "false");
        if (w < 0 || (size_t)w >= cap - n) return 0U;
        n += (size_t)w;
    }
    if (n + 2U > cap) return 0U;
    out[n++] = '}';
    out[n] = '\0';
    return n;
}
