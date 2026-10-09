#include "run_log.h"

#include "mqtt_link_core.h"
#include "telemetry_json.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool used, sent, tomb;   /* tomb: dropped sample batch, kept as a tiny placeholder */
    bool ever_sent;          /* published at least once: only such items can be acked */
    uint8_t kind, unknown, tries;   /* tries: timed-out sends (backend alive) while oldest of its run */
    uint8_t silent;          /* timed-out sends with no backend response at all: backoff only, never abandons */
    uint16_t n_samples;
    uint32_t ord, batch_seq, sent_ms, resp_gen;
    size_t len;
    char run_id[RUNLOG_RUN_ID_MAX + 1U];
    char *data;
} item_t;

#define DROP_RUNS 4U
typedef struct { char run_id[RUNLOG_RUN_ID_MAX + 1U]; uint32_t dropped; } drop_t;
/* UNKNOWN_RUN replies still owed by the previous resend cycle (not counted as a new cycle) */
typedef struct { char run_id[RUNLOG_RUN_ID_MAX + 1U]; uint8_t owed; } unk_t;
#define TOMB_BODY_MAX 256U   /* every sample/event body is allocated at least this big: a tombstone always fits in place */

static const char *TAG = "run_log";
static item_t *s_item;   /* heap, allocated by runlog_init: zero static RAM while the feature is off */
static drop_t s_drop[DROP_RUNS];
static uint8_t s_drop_pos;
static unk_t s_unk[DROP_RUNS];
static uint8_t s_unk_pos;
static uint32_t s_resp_gen;   /* bumps on every valid run/ack: the backend demonstrably answered */
static bool s_test_fail_shrink;
void runlog_test_fail_tomb_shrink(bool on) { s_test_fail_shrink = on; }
static runlog_io_t s_io;
static runlog_stats_t s_stats;
static uint32_t s_ord;
static bool s_was_up;

/* Static, created before any task: the QEMU suite needs no start-up. Held only
 * for short RAM work plus one zero-timeout enqueue; never across a wait. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static void __attribute__((constructor)) lock_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
}
static volatile uint32_t s_lock_takes;
uint32_t runlog_lock_takes(void) { return s_lock_takes; }
static void lock(void) { if (s_lock) { s_lock_takes++; (void)xSemaphoreTake(s_lock, portMAX_DELAY); } }
static void unlock(void) { if (s_lock) (void)xSemaphoreGive(s_lock); }

static const char *suffix_of(uint8_t kind)
{
    switch (kind) {
    case RUNLOG_START: return "run/start";
    case RUNLOG_SAMPLES: return "run/samples";
    case RUNLOG_EVENTS: return "run/events";
    default: return "run/complete";
    }
}

static bool str_of(const char *obj, const char *end, const char *key, char *out, size_t cap);

static void release(item_t *it)
{
    free(it->data);
    memset(it, 0, sizeof(*it));
}

static void note_drop(const char *run_id, uint32_t n)
{
    for (uint8_t i = 0U; i < DROP_RUNS; i++) {
        if (s_drop[i].run_id[0] != '\0' && strcmp(s_drop[i].run_id, run_id) == 0) {
            s_drop[i].dropped += n;
            return;
        }
    }
    drop_t *d = &s_drop[s_drop_pos];   /* oldest entry is recycled */
    s_drop_pos = (uint8_t)((s_drop_pos + 1U) % DROP_RUNS);
    memset(d, 0, sizeof(*d));
    snprintf(d->run_id, sizeof(d->run_id), "%s", run_id);
    d->dropped = n;
}

static uint32_t sample_bytes_locked(void)
{
    uint32_t b = 0U;
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++)
        if (s_item[i].used && s_item[i].kind == RUNLOG_SAMPLES && !s_item[i].tomb) b += (uint32_t)s_item[i].len;
    return b;
}

static uint32_t total_bytes_locked(void)
{
    uint32_t b = 0U;
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) if (s_item[i].used) b += (uint32_t)s_item[i].len;
    return b;
}

/* Overflow policy: the OLDEST sample batch goes, its samples are counted. Its batch_seq
 * stays: the body shrinks in place to {"run_id","boot_id"?,"batch_seq","dropped":true,
 * "samples":k} (a tombstone) so the sequence is contiguous and the cumulative ack can
 * pass it. A tombstone only ever frees bytes, so it needs no reserve to be created. */
static void make_tomb_locked(item_t *it)
{
    char t[TOMB_BODY_MAX], boot[48];
    const char *end = tc_json_object_end(it->data);
    int n;
    if (end != NULL && str_of(it->data, end, "boot_id", boot, sizeof(boot)))
        n = snprintf(t, sizeof(t), "{\"run_id\":\"%s\",\"boot_id\":\"%s\",\"batch_seq\":%lu,\"dropped\":true,\"samples\":%u}",
                     it->run_id, boot, (unsigned long)it->batch_seq, (unsigned)it->n_samples);
    else
        n = snprintf(t, sizeof(t), "{\"run_id\":\"%s\",\"batch_seq\":%lu,\"dropped\":true,\"samples\":%u}",
                     it->run_id, (unsigned long)it->batch_seq, (unsigned)it->n_samples);
    if (n <= 0 || (size_t)n >= sizeof(t)) return;   /* cannot happen (run_id <= 64, boot_id < 48): body kept as is */
    /* The body was allocated >= TOMB_BODY_MAX, so the tombstone is written in place and cannot
     * fail. The big body then really goes back to the heap (len is the real allocation); if the
     * shrink is refused the tombstone is still valid and the old size stays accounted. */
    memcpy(it->data, t, (size_t)n + 1U);
    if (!s_test_fail_shrink && it->len > (size_t)n + 1U) {
        char *c = realloc(it->data, (size_t)n + 1U);
        if (c != NULL) { it->data = c; it->len = (size_t)n; }
    }
    if (it->len < (size_t)n) it->len = (size_t)n;
    it->tomb = true;
    s_stats.tombstones++;
}

static bool drop_oldest_sample_locked(void)
{
    item_t *oldest = NULL;
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
        item_t *it = &s_item[i];
        if (it->used && it->kind == RUNLOG_SAMPLES && !it->tomb && (oldest == NULL || it->ord < oldest->ord)) oldest = it;
    }
    if (oldest == NULL) return false;
    s_stats.dropped_batches++;
    s_stats.dropped_samples += oldest->n_samples;
    note_drop(oldest->run_id, oldest->n_samples);
    make_tomb_locked(oldest);
    return true;
}

/* The backend answered other things yet this run's oldest sample/event stays un-acked for
 * RUNLOG_MAX_RESENDS tries (write_fail, unaddressable body): it would hold a window slot forever.
 * It turns into a tombstone (batch_seq kept so cumulative acks can pass it). Counted and logged.
 * A tombstone, a start and a complete are NEVER released on a timeout (they are tiny). */
static void abandon_locked(item_t *it)
{
    s_stats.resend_abandoned++;
    ESP_LOGW(TAG, "abandon unacked kind=%u run=%s seq=%lu", (unsigned)it->kind, it->run_id, (unsigned long)it->batch_seq);
    if (it->kind == RUNLOG_SAMPLES) {
        s_stats.dropped_batches++;
        s_stats.dropped_samples += it->n_samples;
        note_drop(it->run_id, it->n_samples);
    }
    make_tomb_locked(it);
    it->tries = 0U;
    it->silent = 0U;
    it->sent = false;
}

static unk_t *unk_find(const char *run_id, bool create)
{
    for (uint8_t i = 0U; i < DROP_RUNS; i++)
        if (s_unk[i].run_id[0] != '\0' && strcmp(s_unk[i].run_id, run_id) == 0) return &s_unk[i];
    if (!create) return NULL;
    unk_t *u = &s_unk[s_unk_pos];
    s_unk_pos = (uint8_t)((s_unk_pos + 1U) % DROP_RUNS);
    memset(u, 0, sizeof(*u));
    snprintf(u->run_id, sizeof(u->run_id), "%s", run_id);
    return u;
}

static void unk_clear(const char *run_id)
{
    unk_t *u = unk_find(run_id, false);
    if (u != NULL) u->owed = 0U;
}
/* True when no other item of the same run is older: only that item is blamed for a
 * timeout (the ones behind it are merely waiting for a cumulative ack). */
static bool run_head_locked(const item_t *it)
{
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
        const item_t *o = &s_item[i];
        if (o != it && o->used && o->ord < it->ord && strcmp(o->run_id, it->run_id) == 0) return false;
    }
    return true;
}

static item_t *free_slot_locked(void)
{
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) if (!s_item[i].used) return &s_item[i];
    return NULL;
}

_Static_assert(RUNLOG_ITEM_MAX <= MQTT_LINK_TX_PAYLOAD_MAX, "run ring item must fit the MQTT link payload");

bool runlog_init(const runlog_io_t *io)
{
    lock();
    if (s_item != NULL) for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) release(&s_item[i]);
    if (io == NULL) { free(s_item); s_item = NULL; }
    else if (s_item == NULL) s_item = calloc(RUNLOG_SLOTS, sizeof(item_t));
    memset(s_drop, 0, sizeof(s_drop));
    memset(s_unk, 0, sizeof(s_unk));
    s_unk_pos = 0U;
    s_resp_gen = 0U;
    s_test_fail_shrink = false;
    memset(&s_stats, 0, sizeof(s_stats));
    s_drop_pos = 0U;
    s_ord = 0U;
    s_was_up = false;
    bool ok = (io == NULL) || (s_item != NULL);   /* false: calloc failed, no ring */
    if (io && s_item != NULL) s_io = *io; else memset(&s_io, 0, sizeof(s_io));
    unlock();
    return ok;
}

bool runlog_add(runlog_kind_t kind, const char *run_id, uint32_t batch_seq,
                uint32_t n_samples, const char *json, size_t len)
{
    if (s_item == NULL || run_id == NULL || json == NULL || len == 0U ||
        strlen(run_id) > RUNLOG_RUN_ID_MAX || (unsigned)kind > RUNLOG_TERMINAL) return false;
    bool terminal = kind == RUNLOG_TERMINAL;
    if (terminal) kind = RUNLOG_EVENTS;   /* same wire topic; only the slot class differs */
    bool ok = false;
    item_t *it;
    char *copy;
    lock();
    if (len > RUNLOG_ITEM_MAX) {   /* the link would refuse it forever: fail here, counted */
        s_stats.add_oversize++;
        goto out;
    }
    {
        /* Staggered reserves (one per channel each): samples/events leave 6 slots, a start may use
         * down to 4 left, a terminal event down to 2, a complete down to 0. Tombstones keep their
         * slot, so the active run's start / terminal event / complete are always reachable. A
         * refused item stays in the caller's ring. */
        uint32_t free_slots = 0U, keep = RUNLOG_SLOT_RESERVE;
        if (kind == RUNLOG_COMPLETE) keep = 0U;
        else if (terminal) keep = RUNLOG_RESERVE_PER_CLASS;
        else if (kind == RUNLOG_START) keep = 2U * RUNLOG_RESERVE_PER_CLASS;
        for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) if (!s_item[i].used) free_slots++;
        if (free_slots <= keep) goto out;
    }
    if (kind == RUNLOG_SAMPLES) {
        while (sample_bytes_locked() + len > RUNLOG_SAMPLE_BYTES && drop_oldest_sample_locked()) {}
        if (sample_bytes_locked() + len > RUNLOG_SAMPLE_BYTES) goto out;
    }
    {
        /* Total cap over every kind: oldest samples go first, then the add is refused. */
        uint32_t cap = RUNLOG_TOTAL_BYTES +
                       ((kind == RUNLOG_START || kind == RUNLOG_COMPLETE || terminal) ? RUNLOG_CRITICAL_RESERVE : 0U);
        while (total_bytes_locked() + len > cap && drop_oldest_sample_locked()) {}
        if (total_bytes_locked() + len > cap) {
            s_stats.refused_total_cap++;
            goto out;
        }
    }
    it = free_slot_locked();
    if (it == NULL) goto out;   /* every slot holds start/events/complete/tombstones: caller retries */
    size_t alloc = len + 1U;
    if ((kind == RUNLOG_SAMPLES || kind == RUNLOG_EVENTS) && alloc < TOMB_BODY_MAX) alloc = TOMB_BODY_MAX;
    copy = malloc(alloc);
    if (copy == NULL && drop_oldest_sample_locked()) copy = malloc(alloc);
    if (copy == NULL) goto out;
    memcpy(copy, json, len);
    copy[len] = '\0';
    memset(it, 0, sizeof(*it));
    it->used = true;
    it->kind = (uint8_t)kind;
    it->n_samples = (uint16_t)(kind == RUNLOG_SAMPLES ? n_samples : 0U);
    it->ord = ++s_ord;
    it->batch_seq = batch_seq;
    it->len = len;
    it->data = copy;
    snprintf(it->run_id, sizeof(it->run_id), "%s", run_id);
    s_stats.added++;
    ok = true;
out:
    if (!ok) s_stats.add_refused++;
    unlock();
    return ok;
}

size_t runlog_pump(uint32_t now_ms)
{
    size_t published = 0U;
    lock();
    bool up = s_item != NULL && s_io.connected != NULL && s_io.publish != NULL && s_io.connected();
    if (!up) {
        s_was_up = false;
        unlock();
        return 0U;
    }
    if (!s_was_up) {
        /* (Re)connected: resend everything unacked from the oldest item. */
        s_was_up = true;
        for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
            if (s_item[i].used && s_item[i].sent) { s_item[i].sent = false; s_stats.resent++; }
        }
    }
    uint32_t inflight = 0U;
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
        item_t *it = &s_item[i];
        if (!it->used || !it->sent) continue;
        /* backs off 10/20/40/80 s so a short backend outage is ridden out */
        uint8_t step = it->tries > it->silent ? it->tries : it->silent;
        if (now_ms - it->sent_ms >= (RUNLOG_RESEND_MS << (step < 3U ? step : 3U))) {
            it->sent = false;
            s_stats.resent++;
            unk_clear(it->run_id);   /* replies owed by that send are never coming */
            if (s_resp_gen == it->resp_gen) {
                /* No run/ack of any kind since this send: the backend (or the path to it) is down,
                 * which says nothing about this item. Wait; never count, never abandon. */
                if (it->silent < 255U) it->silent++;
            } else if (run_head_locked(it) && (it->kind == RUNLOG_SAMPLES || it->kind == RUNLOG_EVENTS) && !it->tomb &&
                       ++it->tries >= RUNLOG_MAX_RESENDS) {
                abandon_locked(it);
            }
        } else inflight++;
    }
    while (inflight < RUNLOG_WINDOW) {
        /* Fairness: the next item is the oldest unsent one of the run with the fewest items in
         * flight (ties: the older item), so a stuck or long run cannot starve another run. */
        item_t *next = NULL;
        uint32_t next_in = 0U;
        for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
            item_t *it = &s_item[i];
            if (!it->used || it->sent) continue;
            uint32_t in = 0U;
            bool oldest = true;
            for (uint32_t k = 0U; k < RUNLOG_SLOTS; k++) {
                const item_t *o = &s_item[k];
                if (!o->used || strcmp(o->run_id, it->run_id) != 0) continue;
                if (o->sent) in++;
                else if (o->ord < it->ord) oldest = false;
            }
            if (!oldest) continue;
            if (next == NULL || in < next_in || (in == next_in && it->ord < next->ord)) { next = it; next_in = in; }
        }
        if (next == NULL) break;
        /* A refused enqueue ends this pass so the chosen item is retried first. */
        if (!s_io.publish(suffix_of(next->kind), next->data, 1, false)) break;
        next->sent = true;
        next->ever_sent = true;
        next->sent_ms = now_ms;
        next->resp_gen = s_resp_gen;
        inflight++;
        published++;
        s_stats.published++;
    }
    unlock();
    return published;
}

/* ---- run/ack -------------------------------------------------------------- */

/* Structural lookups (shared with the command parser): only direct members of the
 * top-level object match, once. A key spelled inside a string value or a nested object
 * never counts, and a duplicated key makes the field unusable. */
static bool str_of(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    const char *p = NULL;
    if (tc_json_object_member(obj, end, key, &p) != 1 || p == NULL || *p != '"') return false;
    p++;
    const char *e = p;
    while (e < end && *e != '"' && *e != '\\') e++;   /* ids and states never carry escapes */
    if (e >= end || *e != '"' || (size_t)(e - p) >= cap) return false;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return true;
}

/* 0 = absent, 1 = valid, -1 = present but malformed/duplicated. The literal -1
 * ("nothing stored") is valid: *out = 0 and *none = true. Any other negative is malformed. */
static int u32_of(const char *obj, const char *end, const char *key, uint32_t *out, bool *none)
{
    const char *p = NULL;
    int n = tc_json_object_member(obj, end, key, &p);
    *none = false;
    if (n == 0) return 0;
    if (n == 1 && p != NULL && p[0] == '-' && p[1] == '1' &&
        (p[2] == ',' || p[2] == '}' || p[2] == ' ' || p[2] == '\t' || p[2] == '\r' || p[2] == '\n')) {
        *out = 0U;
        *none = true;
        return 1;
    }
    if (n != 1 || p == NULL || *p < '0' || *p > '9') return -1;
    uint64_t v = 0U;
    size_t digits = 0U;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (++digits > 10U) return -1;
        v = v * 10U + (uint64_t)(*p - '0');
    }
    if ((*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') || v > 0xFFFFFFFFULL) return -1;
    *out = (uint32_t)v;
    return 1;
}

static void drop_run_locked(const char *run_id, bool abandoned)
{
    for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
        item_t *it = &s_item[i];
        if (!it->used || strcmp(it->run_id, run_id) != 0) continue;
        if (abandoned) s_stats.abandoned_items++; else s_stats.acked_items++;
        release(it);
    }
    unk_clear(run_id);
}

void runlog_on_ack(const char *json, size_t len)
{
    char buf[513];
    char run_id[RUNLOG_RUN_ID_MAX + 1U], state[24];
    uint32_t acked = 0U;
    if (s_item == NULL || json == NULL || len == 0U || len >= sizeof(buf)) return;
    memcpy(buf, json, len);
    buf[len] = '\0';
    const char *obj = buf;
    while (*obj == ' ' || *obj == '\t' || *obj == '\r' || *obj == '\n') obj++;
    const char *end = (*obj == '{') ? tc_json_object_end(obj) : NULL;
    bool none = false;
    int acked_rc = end ? u32_of(obj, end, "acked_batch_seq", &acked, &none) : -1;
    bool have_acked = acked_rc == 1 && !none;   /* -1: nothing stored, never trims */
    if (end == NULL || acked_rc < 0 || !str_of(obj, end, "run_id", run_id, sizeof(run_id)) ||
        !str_of(obj, end, "state", state, sizeof(state))) {
        lock(); s_stats.ack_ignored++; unlock();
        return;
    }
    lock();
    if (strcmp(state, "START_OK") == 0 || strcmp(state, "BATCH_OK") == 0 || strcmp(state, "COMPLETE") == 0 ||
        strcmp(state, "COMPLETE_PARTIAL") == 0 || strcmp(state, "INTERRUPTED") == 0 || strcmp(state, "REJECTED") == 0 ||
        strcmp(state, "UNKNOWN_RUN") == 0)
        s_resp_gen++;   /* a valid ack: the backend is alive (any run) */
    if (strcmp(state, "START_OK") == 0 || strcmp(state, "BATCH_OK") == 0) {
        /* Only an ABSENT field means 0 for START_OK; -1 ("nothing stored") never trims. */
        if (strcmp(state, "START_OK") == 0 && acked_rc == 0) acked = 0U;
        else if (!have_acked) { s_stats.ack_ignored++; unlock(); return; }
        for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
            item_t *it = &s_item[i];
            /* ever_sent: a huge ack can never release an item that was not published yet */
            if (it->used && it->ever_sent && it->kind != RUNLOG_COMPLETE && strcmp(it->run_id, run_id) == 0 &&
                it->batch_seq <= acked) {
                s_stats.acked_items++;
                release(it);
            }
        }
    } else if (strcmp(state, "COMPLETE") == 0 || strcmp(state, "COMPLETE_PARTIAL") == 0 ||
               strcmp(state, "INTERRUPTED") == 0) {
        drop_run_locked(run_id, false);   /* run closed: stored, or nothing more to resend */
    } else if (strcmp(state, "REJECTED") == 0) {
        s_stats.rejected_runs++;
        drop_run_locked(run_id, true);
    } else if (strcmp(state, "UNKNOWN_RUN") == 0) {
        bool abandon = false;
        uint32_t n_sent = 0U;
        unk_t *u = unk_find(run_id, false);
        if (u != NULL && u->owed > 0U) {
            u->owed--;   /* late reply of the previous cycle (pump already re-sent): not a new cycle */
        } else {
            for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) {
                item_t *it = &s_item[i];
                if (!it->used || strcmp(it->run_id, run_id) != 0) continue;
                /* One count per resend cycle: only an item sent since the last UNKNOWN_RUN counts, so
                 * the several replies a window of 2 can draw do not add up. */
                if (it->sent) { n_sent++; if (++it->unknown > RUNLOG_MAX_UNKNOWN) abandon = true; }
                it->sent = false;   /* resend from the start */
            }
            if (abandon) drop_run_locked(run_id, true);
            else if (n_sent > 1U && (u = unk_find(run_id, true)) != NULL) u->owed = (uint8_t)(n_sent - 1U);   /* replies still owed by this cycle */
        }
    } else {
        s_stats.ack_ignored++;
    }
    unlock();
}

void runlog_note_superseded(bool closed)
{
    lock();
    s_stats.superseded_runs++;
    if (!closed) s_stats.superseded_unclosed++;
    unlock();
}

uint32_t runlog_dropped_samples(const char *run_id)
{
    uint32_t n = 0U;
    if (run_id == NULL) return 0U;
    lock();
    for (uint8_t i = 0U; i < DROP_RUNS; i++)
        if (s_drop[i].run_id[0] != '\0' && strcmp(s_drop[i].run_id, run_id) == 0) n = s_drop[i].dropped;
    unlock();
    return n;
}

uint32_t runlog_pending(void)
{
    uint32_t n = 0U;
    lock();
    for (uint32_t i = 0U; s_item != NULL && i < RUNLOG_SLOTS; i++) if (s_item[i].used) n++;
    unlock();
    return n;
}

uint32_t runlog_pending_for_run(const char *run_id)
{
    uint32_t n = 0U;
    if (run_id == NULL) return 0U;
    lock();
    for (uint32_t i = 0U; s_item != NULL && i < RUNLOG_SLOTS; i++)
        if (s_item[i].used && strcmp(s_item[i].run_id, run_id) == 0) n++;
    unlock();
    return n;
}

void runlog_stats(runlog_stats_t *out)
{
    if (out == NULL) return;
    lock();
    *out = s_stats;
    out->pending = 0U;
    out->sample_bytes = 0U;
    if (s_item != NULL) {
        for (uint32_t i = 0U; i < RUNLOG_SLOTS; i++) if (s_item[i].used) out->pending++;
        out->sample_bytes = sample_bytes_locked();
        out->total_bytes = total_bytes_locked();
    }
    unlock();
}
