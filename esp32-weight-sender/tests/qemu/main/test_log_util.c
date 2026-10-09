/* Serial-log policy helpers: state-change detector, rate limiter, formatters,
 * Kconfig defaults. Pure logic, no hardware. */

#include <stdint.h>
#include <string.h>

#include "log_util.h"
#include "test_harness.h"
#include "test_log_util.h"
#include "weight_source.h"

/* Kconfig defaults: trace paths off, health period 15 s and inside 0..60. */
_Static_assert(LOG_UTIL_SCALE_TRACE == 0, "SCALE_TRACE_FRAMES must default to n");
_Static_assert(LOG_UTIL_MQTT_TRACE == 0, "MQTT_TRACE_PAYLOADS must default to n");
_Static_assert(CONFIG_HEALTH_SUMMARY_PERIOD_S == 15, "health period default is 15 s");
_Static_assert(CONFIG_HEALTH_SUMMARY_PERIOD_S >= 0 && CONFIG_HEALTH_SUMMARY_PERIOD_S <= 60,
               "health period range");
#ifdef CONFIG_SCALE_TRACE_FRAMES
#error "CONFIG_SCALE_TRACE_FRAMES must be off by default"
#endif
#ifdef CONFIG_MQTT_TRACE_PAYLOADS
#error "CONFIG_MQTT_TRACE_PAYLOADS must be off by default"
#endif
#ifdef CONFIG_WEIGHT_DEMO_LOG_SCALE_FRAMES
#error "CONFIG_WEIGHT_DEMO_LOG_SCALE_FRAMES must be off by default"
#endif

static void test_change(void)
{
    log_change_t c;
    log_change_init(&c);
    int32_t prev = 99;

    test_check(log_on_change(&c, 1, &prev) && prev == LOG_CHANGE_NONE,
               "log_change_first_observation_reports");
    test_check(!log_on_change(&c, 1, &prev), "log_change_same_state_silent");
    test_check(!log_on_change(&c, 1, NULL), "log_change_same_state_silent_again");
    test_check(log_on_change(&c, 2, &prev) && prev == 1, "log_change_reports_old_value");
    test_check(!log_on_change(&c, 2, &prev), "log_change_no_repeat_after_change");
    test_check(log_on_change(&c, 1, &prev) && prev == 2, "log_change_flip_back_reports");
    test_check(!log_on_change(NULL, 1, &prev), "log_change_null_safe");
}

static void test_ratelimit(void)
{
    log_ratelimit_t r;
    log_ratelimit_init(&r);
    uint32_t n = 0;

    test_check(log_ratelimit_event(&r, 1000U, 30000U, &n) && n == 1U,
               "ratelimit_first_event_immediate");
    test_check(!log_ratelimit_event(&r, 2000U, 30000U, &n), "ratelimit_second_suppressed");
    test_check(!log_ratelimit_event(&r, 20000U, 30000U, &n), "ratelimit_inside_interval_suppressed");
    test_check(!log_ratelimit_event(&r, 30999U, 30000U, &n), "ratelimit_just_before_interval");
    test_check(log_ratelimit_event(&r, 31000U, 30000U, &n) && n == 4U,
               "ratelimit_summary_counts_suppressed");
    test_check(!log_ratelimit_event(&r, 31001U, 30000U, &n), "ratelimit_window_restarts");
    test_check(log_ratelimit_event(&r, 61000U, 30000U, &n) && n == 2U,
               "ratelimit_next_summary_count");

    log_ratelimit_init(&r);
    test_check(log_ratelimit_event(&r, 5U, 1000U, &n) && n == 1U, "ratelimit_reset_rearms_immediate");
}

static void test_ratelimit_wrap(void)
{
    log_ratelimit_t r;
    log_ratelimit_init(&r);
    uint32_t n = 0;
    uint32_t t0 = UINT32_MAX - 5000U; /* 5 s before the 32-bit ms counter wraps */

    test_check(log_ratelimit_event(&r, t0, 30000U, &n), "ratelimit_wrap_first");
    test_check(!log_ratelimit_event(&r, UINT32_MAX, 30000U, &n), "ratelimit_wrap_before_rollover");
    test_check(!log_ratelimit_event(&r, 10000U, 30000U, &n), "ratelimit_wrap_after_rollover_suppressed");
    /* 5001 + 24999 = 30000 ms after t0, now numerically tiny. */
    test_check(!log_ratelimit_event(&r, 24998U, 30000U, &n), "ratelimit_wrap_one_ms_early");
    test_check(log_ratelimit_event(&r, 25000U, 30000U, &n) && n == 4U,
               "ratelimit_wrap_emits_after_interval");
}

static void test_formatters(void)
{
    char b[96];

    test_check(log_fmt_age_tenths(b, sizeof(b), 3200U) > 0U && strcmp(b, "3.2s") == 0,
               "fmt_age_3_2s");
    test_check(log_fmt_age_tenths(b, sizeof(b), 10090U) > 0U && strcmp(b, "10.1s") == 0,
               "fmt_age_rounds_to_tenth");
    test_check(log_fmt_age_tenths(b, 3U, 3200U) == 0U, "fmt_age_truncation_reported");

    test_check(log_fmt_weight_kg(b, sizeof(b), 4823) > 0U && strcmp(b, "4.823kg") == 0,
               "fmt_weight_4_823kg");
    test_check(log_fmt_weight_kg(b, sizeof(b), -500) > 0U && strcmp(b, "-0.500kg") == 0,
               "fmt_weight_negative_keeps_sign");
    test_check(log_fmt_weight_kg(b, sizeof(b), INT32_MIN) > 0U &&
               strcmp(b, "-2147483.648kg") == 0, "fmt_weight_int_min");

    test_check(log_fmt_transition(b, sizeof(b), "ONLINE", "STALE", true, 3200U) > 0U &&
               strcmp(b, "ONLINE -> STALE age=3.2s") == 0, "fmt_transition_online_stale");
    test_check(log_fmt_transition(b, sizeof(b), "STALE", "OFFLINE", true, 10100U) > 0U &&
               strcmp(b, "STALE -> OFFLINE age=10.1s") == 0, "fmt_transition_stale_offline");
    test_check(log_fmt_transition(b, sizeof(b), "WAITING", "ONLINE", false, 0U) > 0U &&
               strcmp(b, "WAITING -> ONLINE") == 0, "fmt_transition_without_age");
}

static void test_health(void)
{
    char b[160];
    log_health_t h = {
        .uptime_s = 452U, .heap_kb = 181U, .mqtt_up = true, .scale_state = "ONLINE",
        .weight_valid = true, .weight_g = 4823,
        .rx = 4821U, .valid = 4817U, .bad = 4U, .pub = 921U, .queue = 0U,
        .age_valid = true, .age_ms = 70U,
    };
    test_check(log_fmt_health(b, sizeof(b), &h) > 0U &&
               strcmp(b, "up=452s heap=181KB mqtt=UP scale=ONLINE weight=4.823kg "
                         "rx=4821 valid=4817 bad=4 pub=921 q=0 age=0.07s") == 0,
               "health_exact_string");

    /* Stale scale: no weight shown, never 0.000 as a fallback. */
    h.weight_valid = false;
    h.weight_g = 0;
    h.scale_state = "STALE";
    h.mqtt_up = false;
    h.age_ms = 3200U;
    test_check(log_fmt_health(b, sizeof(b), &h) > 0U &&
               strcmp(b, "up=452s heap=181KB mqtt=DOWN scale=STALE weight=n/a "
                         "rx=4821 valid=4817 bad=4 pub=921 q=0 age=3.20s") == 0,
               "health_weight_na_when_stale");
    test_check(strstr(b, "0.000") == NULL, "health_never_prints_zero_fallback");

    /* Never had a valid sample: weight and age both n/a. */
    memset(&h, 0, sizeof(h));
    h.uptime_s = 3U;
    h.heap_kb = 200U;
    h.scale_state = "WAITING";
    test_check(log_fmt_health(b, sizeof(b), &h) > 0U &&
               strcmp(b, "up=3s heap=200KB mqtt=DOWN scale=WAITING weight=n/a "
                         "rx=0 valid=0 bad=0 pub=0 q=0 age=n/a") == 0,
               "health_na_when_no_valid_sample");

    /* A genuine 0 g reading is a real value and is shown as such. */
    h.weight_valid = true;
    h.weight_g = 0;
    h.age_valid = true;
    h.age_ms = 40U;
    test_check(log_fmt_health(b, sizeof(b), &h) > 0U && strstr(b, "weight=0.000kg") != NULL,
               "health_real_zero_is_shown");

    test_check(log_fmt_health(b, 20U, &h) == 0U, "health_truncation_reported");
    test_check(log_fmt_health(NULL, 0U, &h) == 0U, "health_null_safe");
}

/* weight_source feeds the health line: no valid sample must mean no weight. */
static void test_weight_source_feed(void)
{
    int32_t g = 1234;
    weight_source_start();
    test_check(!weight_source_last_weight_g(&g), "health_feed_no_weight_before_first_sample");
    test_check(g == 1234, "health_feed_leaves_output_untouched_when_absent");
    test_check(!weight_source_last_weight_g(NULL), "health_feed_null_safe");
    test_check(strcmp(cas_link_state_short(CAS_LINK_WAITING), "WAITING") == 0 &&
               strcmp(cas_link_state_short(CAS_LINK_ONLINE), "ONLINE") == 0 &&
               strcmp(cas_link_state_short(CAS_LINK_STALE), "STALE") == 0 &&
               strcmp(cas_link_state_short(CAS_LINK_OFFLINE), "OFFLINE") == 0,
               "link_state_short_names");
}

void test_log_util_run(void)
{
    test_weight_source_feed();
    test_change();
    test_ratelimit();
    test_ratelimit_wrap();
    test_formatters();
    test_health();
}
