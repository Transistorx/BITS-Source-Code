#ifndef LOG_UTIL_H
#define LOG_UTIL_H

/*
 * Serial-log policy helpers. Pure logic (no hardware, no locks, no allocation)
 * so the QEMU suite can test it and so it is safe to call from any task.
 *
 * Rules the firmware follows with these:
 *   - log transitions, not repeated states (log_on_change)
 *   - first occurrence immediately, then at most one summary per interval with
 *     a count (log_ratelimit_event)
 *   - never log per RS-485 frame, per parsed sample or per MQTT payload at INFO
 *
 * Tag set (exact strings): SYS BOARD NET MQTT RS485 SCALE WEIGHT CMD QUEUE.
 * provisioning (PROV), wifi_manager and nvs_config keep their own tags: they
 * are duplicated in the relay project.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Kconfig mirrors (defaults match components/log_util/Kconfig.projbuild) ---- */

#ifndef CONFIG_HEALTH_SUMMARY_PERIOD_S
#define CONFIG_HEALTH_SUMMARY_PERIOD_S 15
#endif
#define LOG_UTIL_HEALTH_PERIOD_S CONFIG_HEALTH_SUMMARY_PERIOD_S

/* CONFIG_WEIGHT_DEMO_LOG_SCALE_FRAMES is the deprecated name of
 * CONFIG_SCALE_TRACE_FRAMES and still enables the same trace. */
#if defined(CONFIG_SCALE_TRACE_FRAMES) || defined(CONFIG_WEIGHT_DEMO_LOG_SCALE_FRAMES)
#define LOG_UTIL_SCALE_TRACE 1
#else
#define LOG_UTIL_SCALE_TRACE 0
#endif

#if defined(CONFIG_MQTT_TRACE_PAYLOADS)
#define LOG_UTIL_MQTT_TRACE 1
#else
#define LOG_UTIL_MQTT_TRACE 0
#endif

/* ---- State-change detector ------------------------------------------------ */

#define LOG_CHANGE_NONE (-1)

typedef struct {
    int32_t last;
    bool seen;
} log_change_t;

void log_change_init(log_change_t *c);

/* True when value differs from the previous observation, or on the first one.
 * *prev (optional) receives the previous value, LOG_CHANGE_NONE on the first.
 * The caller logs "A -> B" once; an unchanged value returns false. */
bool log_on_change(log_change_t *c, int32_t value, int32_t *prev);

/* ---- Rate limiter ---------------------------------------------------------- */

typedef struct {
    uint32_t last_ms;   /* when a line was last emitted */
    uint32_t pending;   /* events folded since then */
    bool armed;         /* false until the first event */
} log_ratelimit_t;

void log_ratelimit_init(log_ratelimit_t *r);

/* Register one occurrence. Returns true when the caller should log now:
 * the first event immediately (*count = 1), then once an interval has passed,
 * with *count = events since the previous line. Wrap-safe for uint32_t ms. */
bool log_ratelimit_event(log_ratelimit_t *r, uint32_t now_ms, uint32_t interval_ms,
                         uint32_t *count);

/* ---- Formatters (return length, 0 on truncation/bad args) ------------------- */

/* "3.2s" (tenths, rounded). */
size_t log_fmt_age_tenths(char *buf, size_t cap, uint32_t age_ms);

/* "-1.250kg" from grams. */
size_t log_fmt_weight_kg(char *buf, size_t cap, int32_t grams);

/* "ONLINE -> STALE age=3.2s", or without " age=" when have_age is false. */
size_t log_fmt_transition(char *buf, size_t cap, const char *from, const char *to,
                          bool have_age, uint32_t age_ms);

typedef struct {
    uint32_t uptime_s;
    uint32_t heap_kb;
    bool mqtt_up;
    const char *scale_state;  /* WAITING / ONLINE / STALE / OFFLINE */
    bool weight_valid;        /* true only for a real, fresh, valid sample */
    int32_t weight_g;
    uint32_t rx, valid, bad, pub, queue;
    bool age_valid;           /* a valid sample has been seen since boot */
    uint32_t age_ms;
} log_health_t;

/* "up=452s heap=181KB mqtt=UP scale=ONLINE weight=4.823kg rx=4821 valid=4817
 * bad=4 pub=921 q=0 age=0.07s". weight=n/a unless weight_valid, age=n/a unless
 * age_valid: there is no zero fallback. */
size_t log_fmt_health(char *buf, size_t cap, const log_health_t *h);

/* ---- Per-channel RS-485 diagnostics --------------------------------------- */

typedef struct {
    uint8_t ch;               /* 1 or 2 (board channel number) */
    int uart;                 /* ESP-IDF UART number */
    bool selected;            /* this is the configured scale channel */
    uint32_t bytes;           /* raw RX bytes since boot */
    uint32_t bytes_per_s;     /* over the last summary interval */
    uint32_t frames;          /* complete lines seen */
    uint32_t valid;           /* accepted CAS frames */
    uint32_t valid_st, valid_us; /* accepted frames with ST / US status */
    uint32_t rej_len, rej_hdr, rej_stat, rej_field, rej_other;
    uint32_t rej_term;        /* over-long line, terminator missing */
    uint32_t uart_faults;     /* fifo/ring/break/parity/framing/evq/read */
    uint32_t loop_gap_ms;     /* longest reader-task loop gap (stall detector) */
    bool has_weight;          /* a frame was accepted on THIS channel */
    int32_t weight_g;
    bool stable;
    bool have_age;
    uint32_t age_ms;          /* age of that accepted frame */
    const char *link;         /* reader link state: ONLINE / OFFLINE */
    bool have_seq;            /* only the selected channel has a cas_seq */
    uint32_t cas_seq;
    uint32_t rej_format;      /* decimal-point format differs from the established one */
    uint32_t rej_pad;         /* strict pad check failed */
    uint32_t overload;        /* OL frames (never published as weight) */
    uint32_t stalls;          /* reader-loop stalls (ring flushed, resynced) */
    uint32_t resyncs;         /* discard-until-terminator recoveries */
    uint32_t embedded_lf;     /* LF bytes consumed as data inside a CI-150A body */
} log_chan_t;

/* Longest line log_fmt_chan_summary can produce is well under this; callers
 * size their buffer with it so a worst-case counter set is never dropped. */
#define LOG_CHAN_SUMMARY_MAX 512U

/* "RX CH1/UART1 sel=Y bytes=1234 (96B/s) frames=56 valid=50 rej[len=0 hdr=0
 * stat=0 field=6 other=0 term=0] faults=0 last=0.660kg ST age=120ms link=ONLINE
 * cas_seq=50". last=n/a / age=n/a when nothing was accepted on the channel. */
size_t log_fmt_chan_summary(char *buf, size_t cap, const log_chan_t *c);

typedef struct {
    uint8_t ch;
    int uart;
    bool selected;
    bool to_online;
    const char *reason;       /* short reason text */
    uint32_t bytes, valid, rejected;
} log_link_transition_t;

/* "CH1/UART1 link OFFLINE -> ONLINE reason=confirmed bytes=1234 valid=50
 * rejected=3 sel=Y". */
size_t log_fmt_link_transition(char *buf, size_t cap, const log_link_transition_t *t);

/* ---- RAW CAPTURE (bench diagnostic) ---------------------------------------- */

#define LOG_RAW_TAIL_MAX 40U
#define LOG_RAW_LINE_MAX 320U

typedef struct {
    uint8_t ch;
    int uart;
    const uint8_t *tail;      /* last raw bytes received */
    size_t tail_len;          /* <= LOG_RAW_TAIL_MAX */
    bool have_pad_lamp;       /* a valid frame has been accepted */
    uint8_t pad;              /* byte 17 of the last valid frame */
    uint8_t lamp;             /* byte 7 (lamp octet) of the last valid frame */
    uint32_t embedded_lf;
} log_raw_t;

/* "RAW CH1/UART1 last=22B hex=53 54 2C ... ascii=\"ST,GS,..\" pad=0x00 lamp=0x00
 * elf=0". Non-printable bytes are '.' in the ascii part. 0 on truncation. */
size_t log_fmt_raw_capture(char *buf, size_t cap, const log_raw_t *r);

/* "RAW CH1/UART1 first-OL-frame=20B hex=.. ascii=\"..\"" */
size_t log_fmt_raw_ol(char *buf, size_t cap, uint8_t ch, int uart,
                      const uint8_t *frame, size_t len);

/* Applies per-tag runtime levels: INFO by default; DEBUG for RS485 / MQTT only
 * when SCALE_TRACE_FRAMES / MQTT_TRACE_PAYLOADS is on (the build also needs
 * CONFIG_LOG_MAXIMUM_LEVEL >= DEBUG for those lines to exist). */
void log_util_apply_levels(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_UTIL_H */
