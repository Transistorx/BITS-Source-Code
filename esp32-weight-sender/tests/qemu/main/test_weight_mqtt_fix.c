#include "test_weight_mqtt_fix.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_link_core.h"
#include "test_harness.h"

#define FX_DEV "bits-a4cf12ab34cd"
#define FX_BOOT "a1b2c3d4"
#define FX_TEL "cas/" FX_DEV "/telemetry/weight"
#define FX_CTL "cas/" FX_DEV "/weight/ctl"
#define FX_NOT_YET (-1)

static mqtt_link_queues_t s_fq;
static mqtt_item_t s_fit;

static uint32_t fx_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool fx_wait_flag(atomic_int *flag, uint32_t limit_ms)
{
    for (uint32_t w = 0U; atomic_load(flag) == FX_NOT_YET && w < limit_ms; w += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return atomic_load(flag) != FX_NOT_YET;
}

static void fx_init(mqtt_link_queues_t *q)
{
    memset(q, 0, sizeof(*q));
    (void)mqtt_link_queues_init(q, 4U, 6U);
}

static void fx_put(mqtt_link_queues_t *q, const char *data, uint32_t now)
{
    (void)mqtt_link_queue_put_weight(q, MQTT_LINK_WEIGHT_SLOT_TEL, FX_TEL, data, now);
}

static mqtt_link_weight_t fx_w(uint32_t uptime, uint32_t age)
{
    mqtt_link_weight_t w = {
        .uptime_ms = uptime, .scale_id = "SCALE1", .src_uart = 1U, .weight_g = 2500, .stable = true,
        .age_ms = age, .cas_seq = 77U, .source = "CAS_RS485",
    };
    return w;
}

static mqtt_enq_result_t fx_sample(mqtt_link_queues_t *q, unsigned slot, uint32_t seq,
                                   const mqtt_link_weight_t *w, bool mid, uint32_t now)
{
    return mqtt_link_queue_put_weight_sample(q, slot, slot == MQTT_LINK_WEIGHT_SLOT_CTL ? FX_CTL : FX_TEL,
                                             FX_BOOT, seq, w, mid, now);
}

static void test_future_stamp(void)
{
    fx_init(&s_fq);
    fx_put(&s_fq, "{\"f\":1}", 5000U);
    test_check(!mqtt_link_queue_next(&s_fq, &s_fit, 0U, 4000U, 1000U),
               "REQ-WMQ-04_fix_future_stamp_beyond_tolerance_not_delivered");
    test_check(atomic_load(&s_fq.weight_dropped_future) == 1U && atomic_load(&s_fq.weight_dropped_stale) == 0U,
               "REQ-WMQ-04_fix_future_stamp_counted_as_future");
    fx_put(&s_fq, "{\"f\":2}", 5000U);
    test_check(mqtt_link_queue_next(&s_fq, &s_fit, 0U, 5000U - MQTT_LINK_WEIGHT_FUTURE_TOL_MS, 1000U) &&
                   strcmp(s_fit.data, "{\"f\":2}") == 0,
               "REQ-WMQ-04_fix_future_stamp_within_tolerance_delivered");
    fx_put(&s_fq, "{\"f\":3}", 5000U);
    test_check(!mqtt_link_queue_next(&s_fq, &s_fit, 0U, 5000U - MQTT_LINK_WEIGHT_FUTURE_TOL_MS - 1U, 1000U) &&
                   atomic_load(&s_fq.weight_dropped_future) == 2U,
               "REQ-WMQ-04_fix_future_stamp_one_ms_past_tolerance_dropped");
    fx_put(&s_fq, "{\"f\":4}", 0x00000010U);
    test_check(mqtt_link_queue_next(&s_fq, &s_fit, 0U, 0xFFFFFFF0U, 1000U) && strcmp(s_fit.data, "{\"f\":4}") == 0,
               "REQ-WMQ-04_fix_future_stamp_wrap_within_tolerance_delivered");
    fx_put(&s_fq, "{\"f\":5}", 0x80000000U);
    test_check(!mqtt_link_queue_next(&s_fq, &s_fit, 0U, 0U, 1000U) &&
                   atomic_load(&s_fq.weight_dropped_future) + atomic_load(&s_fq.weight_dropped_stale) == 3U,
               "REQ-WMQ-04_fix_half_range_stamp_never_fresh");
    (void)mqtt_link_queues_deinit(&s_fq);
}

static void test_publish_time_age(void)
{
    static char exp[MQTT_LINK_PAYLOAD_MAX + 1U];
    fx_init(&s_fq);

    mqtt_link_weight_t w = fx_w(10000U, 200U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 5U, &w, true, 10000U) == MQTT_ENQ_OK,
               "REQ-WMQ-07_fix_put_sample_ok");
    bool got = mqtt_link_queue_next(&s_fq, &s_fit, 0U, 10900U, 1000U);
    mqtt_link_weight_t e = fx_w(10000U, 1100U);
    size_t n = mqtt_link_weight_json(exp, sizeof(exp), FX_BOOT, 5U, &e, true);
    test_check(got && s_fit.kind == MQTT_ITEM_PUB && n > 0U && s_fit.len == n && strcmp(s_fit.data, exp) == 0 &&
                   strcmp(s_fit.topic, FX_TEL) == 0 && s_fit.qos == 0U && !s_fit.retain,
               "REQ-WMQ-07_fix_age_ms_computed_at_publish_time");
    test_check(got && strstr(s_fit.data, "\"age_ms\":1100,") != NULL && strstr(s_fit.data, "\"uptime_ms\":10000,") != NULL,
               "REQ-WMQ-07_fix_age_includes_queue_dwell");

    w = fx_w(20000U, 0U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_CTL, 9U, &w, false, 20000U) == MQTT_ENQ_OK &&
                   mqtt_link_queue_next(&s_fq, &s_fit, 0U, 20050U, 1000U) &&
                   strstr(s_fit.data, "\"age_ms\":50,") != NULL && strstr(s_fit.data, "message_id") == NULL &&
                   strstr(s_fit.data, "\"seq\":9,") != NULL && strcmp(s_fit.topic, FX_CTL) == 0,
               "REQ-WMQ-07_fix_ctl_slot_age_at_publish_no_message_id");

    w = fx_w(30000U, 0U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 10U, &w, true, 30000U) == MQTT_ENQ_OK &&
                   mqtt_link_queue_next(&s_fq, &s_fit, 0U, 29950U, 1000U) &&
                   strstr(s_fit.data, "\"age_ms\":0,") != NULL,
               "REQ-WMQ-07_fix_sample_skew_within_tolerance_age_zero");

    uint32_t stale0 = atomic_load(&s_fq.weight_dropped_stale);
    w = fx_w(40000U, 2900U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 11U, &w, true, 40000U) == MQTT_ENQ_OK &&
                   !mqtt_link_queue_next(&s_fq, &s_fit, 0U, 40200U, 1000U) &&
                   atomic_load(&s_fq.weight_dropped_stale) == stale0 + 1U,
               "REQ-WMQ-06_fix_publish_age_over_3000_dropped_and_counted");
    w = fx_w(50000U, 2900U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 12U, &w, true, 50000U) == MQTT_ENQ_OK &&
                   mqtt_link_queue_next(&s_fq, &s_fit, 0U, 50100U, 1000U) &&
                   strstr(s_fit.data, "\"age_ms\":3000,") != NULL,
               "REQ-WMQ-06_fix_publish_age_exactly_3000_delivered");

    w = fx_w(60000U, 0U);
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 13U, &w, true, 60000U) == MQTT_ENQ_OK &&
                   fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 14U, &w, true, 60010U) == MQTT_ENQ_OK &&
                   mqtt_link_queue_next(&s_fq, &s_fit, 0U, 60020U, 1000U) &&
                   strstr(s_fit.data, "\"seq\":14,") != NULL &&
                   !mqtt_link_queue_next(&s_fq, &s_fit, 0U, 60020U, 1000U),
               "REQ-WMQ-04_fix_sample_mailbox_latest_wins");

    mqtt_link_weight_t worst = fx_w(4294967295U, 1000U);
    worst.src_uart = 2U;
    worst.weight_g = -2147483647 - 1;
    worst.cas_seq = 4294967295U;
    test_check(fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 4294967295U, &worst, true, 70000U) == MQTT_ENQ_OK &&
                   mqtt_link_queue_next(&s_fq, &s_fit, 0U, 70000U, 1000U) && s_fit.len <= MQTT_LINK_PAYLOAD_MAX &&
                   strlen(s_fit.data) == s_fit.len,
               "REQ-WMQ-12_fix_worst_case_sample_renders_within_512");

    mqtt_link_weight_t bad = fx_w(1U, 1U);
    bad.scale_id = "bad id";
    bool r1 = fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 1U, &bad, true, 1U) == MQTT_ENQ_INVALID;
    bad = fx_w(1U, 1U);
    bad.src_uart = 3U;
    bool r2 = fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 1U, &bad, true, 1U) == MQTT_ENQ_INVALID;
    bad = fx_w(1U, 1U);
    bad.source = "SOURCE_NAME_LONGER_THAN_THIRTY_ONE_CHARS";
    bool r3 = fx_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, 1U, &bad, true, 1U) == MQTT_ENQ_INVALID;
    bool r4 = mqtt_link_queue_put_weight_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, FX_TEL, "XYZ", 1U, &w, true,
                                                1U) == MQTT_ENQ_INVALID;
    bool r5 = mqtt_link_queue_put_weight_sample(&s_fq, MQTT_LINK_WEIGHT_SLOTS, FX_TEL, FX_BOOT, 1U, &w, true,
                                                1U) == MQTT_ENQ_INVALID;
    bool r6 = mqtt_link_queue_put_weight_sample(&s_fq, MQTT_LINK_WEIGHT_SLOT_TEL, FX_TEL, FX_BOOT, 1U, NULL, true,
                                                1U) == MQTT_ENQ_INVALID;
    test_check(r1 && r2 && r3 && r4 && r5 && r6 && mqtt_link_queue_pending(&s_fq) == 0U,
               "REQ-WMQ-12_fix_put_sample_rejects_bad_input_nothing_queued");
    (void)mqtt_link_queues_deinit(&s_fq);
}

typedef struct {
    mqtt_link_handoff_t *h;
    mqtt_link_queues_t *q;
    uint32_t work_ms;
    atomic_int exit_ret;
    atomic_int freed;
} fx_worker_t;

static void fx_worker(void *arg)
{
    fx_worker_t *c = (fx_worker_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(c->work_ms));
    bool stopper_owns = mqtt_link_handoff_task_exit(c->h);
    if (!stopper_owns && c->q != NULL) atomic_store(&c->freed, mqtt_link_queues_deinit(c->q) ? 1 : 0);
    atomic_store(&c->exit_ret, stopper_owns ? 1 : 0);
    vTaskDelete(NULL);
}

static bool fx_spawn(fx_worker_t *c, mqtt_link_handoff_t *h, mqtt_link_queues_t *q, uint32_t work_ms)
{
    c->h = h;
    c->q = q;
    c->work_ms = work_ms;
    atomic_store(&c->exit_ret, FX_NOT_YET);
    atomic_store(&c->freed, FX_NOT_YET);
    mqtt_link_handoff_init(h);
    return xTaskCreate(fx_worker, "fx_worker", 3072, c, 3, NULL) == pdPASS;
}

static void test_handoff(void)
{
    static mqtt_link_handoff_t h;
    static fx_worker_t c;

    test_check(fx_spawn(&c, &h, NULL, 20U), "REQ-WMQ-52_fix_handoff_spawn_prompt");
    bool owns = mqtt_link_handoff_wait(&h, 300U);
    test_check(owns && fx_wait_flag(&c.exit_ret, 500U) && atomic_load(&c.exit_ret) == 1,
               "REQ-WMQ-52_fix_prompt_exit_stopper_owns_teardown");
    test_check(mqtt_link_handoff_wait(&h, 0U), "REQ-WMQ-52_fix_wait_after_done_is_immediate");

    fx_init(&s_fq);
    test_check(fx_spawn(&c, &h, &s_fq, 400U), "REQ-WMQ-52_fix_handoff_spawn_stuck");
    uint32_t t0 = fx_now_ms();
    owns = mqtt_link_handoff_wait(&h, 100U);
    uint32_t dt = fx_now_ms() - t0;
    test_check(!owns && dt < 250U, "REQ-WMQ-52_fix_stuck_task_wait_is_bounded_and_not_owned");
    test_check(s_fq.rx != NULL && s_fq.doorbell != NULL && atomic_load(&c.exit_ret) == FX_NOT_YET,
               "REQ-WMQ-52_fix_stuck_task_queues_not_freed_by_stopper");
    test_check(fx_wait_flag(&c.exit_ret, 1000U) && atomic_load(&c.exit_ret) == 0 && atomic_load(&c.freed) == 1 &&
                   s_fq.rx == NULL && s_fq.doorbell == NULL,
               "REQ-WMQ-52_fix_late_task_owns_and_completes_teardown");

    int single_owner = 1;
    for (uint32_t i = 0U; i < 12U; i++) {
        if (!fx_spawn(&c, &h, NULL, 30U + 5U * i)) {
            single_owner = 0;
            break;
        }
        bool s = mqtt_link_handoff_wait(&h, 60U);
        if (!fx_wait_flag(&c.exit_ret, 1000U)) {
            single_owner = 0;
            break;
        }
        int t = atomic_load(&c.exit_ret);
        if (!((s && t == 1) || (!s && t == 0))) single_owner = 0;
    }
    test_check(single_owner == 1, "REQ-WMQ-52_fix_exactly_one_teardown_owner_near_timeout");
}

typedef struct {
    mqtt_link_queues_t *q;
    atomic_int ret;
} fx_cons_t;

static void fx_consumer(void *arg)
{
    fx_cons_t *c = (fx_cons_t *)arg;
    static mqtt_item_t it;
    bool got = mqtt_link_queue_next(c->q, &it, 3000U, 0U, 1000U);
    atomic_store(&c->ret, got ? 1 : 0);
    vTaskDelete(NULL);
}

static void test_deinit_waits_for_consumer(void)
{
    static fx_cons_t c;
    fx_init(&s_fq);
    c.q = &s_fq;
    atomic_store(&c.ret, FX_NOT_YET);
    test_check(xTaskCreate(fx_consumer, "fx_cons", 3072, &c, 5, NULL) == pdPASS, "REQ-WMQ-52_fix_consumer_spawn");
    vTaskDelay(pdMS_TO_TICKS(50));
    test_check(atomic_load(&c.ret) == FX_NOT_YET, "REQ-WMQ-52_fix_consumer_blocked_on_doorbell");
    uint32_t t0 = fx_now_ms();
    bool freed = mqtt_link_queues_deinit(&s_fq);
    uint32_t dt = fx_now_ms() - t0;
    int seen = atomic_load(&c.ret);
    test_check(freed && seen == 0 && dt < 300U, "REQ-WMQ-52_fix_deinit_returns_after_blocked_consumer_left");
    test_check(s_fq.rx == NULL && s_fq.pub == NULL && s_fq.doorbell == NULL, "REQ-WMQ-52_fix_deinit_clears_handles");
    test_check(mqtt_link_queue_put_weight(&s_fq, 1U, FX_TEL, "{}", 1U) == MQTT_ENQ_INVALID &&
                   mqtt_link_queue_push_rx(&s_fq, "t", 1U, "d", 1U, 1U, false, 0U) == MQTT_ENQ_INVALID &&
                   !mqtt_link_queue_next(&s_fq, &s_fit, 0U, 0U, 1000U) && mqtt_link_queue_pending(&s_fq) == 0U,
               "REQ-WMQ-52_fix_closed_queues_refuse_everyone");
}

typedef struct {
    mqtt_link_queues_t *q;
    atomic_bool stop;
    atomic_bool done;
    atomic_uint ok;
    atomic_uint refused;
} fx_prod_t;

static void fx_producer(void *arg)
{
    fx_prod_t *p = (fx_prod_t *)arg;
    mqtt_link_weight_t w = fx_w(1U, 0U);
    uint32_t n = 0U;
    while (!atomic_load(&p->stop)) {
        mqtt_enq_result_t a = mqtt_link_queue_put_weight(p->q, n & 1U, FX_TEL, "{\"p\":1}", n);
        mqtt_enq_result_t b = fx_sample(p->q, (n + 1U) & 1U, n, &w, true, n);
        mqtt_enq_result_t c = mqtt_link_queue_push_pub(p->q, FX_TEL, "{}", 0, false);
        mqtt_enq_result_t d = mqtt_link_queue_push_rx(p->q, "t", 1U, "d", 1U, 1U, false, n);
        (void)mqtt_link_queue_pending(p->q);
        if (a == MQTT_ENQ_OK || b == MQTT_ENQ_OK || c == MQTT_ENQ_OK || d == MQTT_ENQ_OK) {
            atomic_fetch_add(&p->ok, 1U);
        } else if (a == MQTT_ENQ_INVALID && b == MQTT_ENQ_INVALID) {
            atomic_fetch_add(&p->refused, 1U);
        }
        n++;
        if ((n & 31U) == 0U) {
            vTaskDelay(1);
        } else {
            taskYIELD();
        }
    }
    atomic_store(&p->done, true);
    vTaskDelete(NULL);
}

static void test_producers_during_teardown(void)
{
    static fx_prod_t p[2];
    static mqtt_item_t drain;
    fx_init(&s_fq);
    (void)mqtt_link_queues_deinit(&s_fq);
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (int i = 0; i < 2; i++) {
        p[i].q = &s_fq;
        atomic_store(&p[i].stop, false);
        atomic_store(&p[i].done, false);
        atomic_store(&p[i].ok, 0U);
        atomic_store(&p[i].refused, 0U);
    }
    bool spawned = xTaskCreatePinnedToCore(fx_producer, "fx_prod0", 3072, &p[0], 2, NULL, 0) == pdPASS &&
                   xTaskCreatePinnedToCore(fx_producer, "fx_prod1", 3072, &p[1], 2, NULL,
                                           portNUM_PROCESSORS > 1 ? 1 : 0) == pdPASS;
    test_check(spawned, "REQ-WMQ-52_fix_producers_spawn");
    int all_freed = 1;
    int all_cleared = 1;
    for (int cycle = 0; cycle < 25; cycle++) {
        if (!mqtt_link_queues_init(&s_fq, 4U, 6U)) all_freed = 0;
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)mqtt_link_queue_next(&s_fq, &drain, 0U, 0U, 1000U);
        if (!mqtt_link_queues_deinit(&s_fq)) all_freed = 0;
        if (s_fq.rx != NULL || s_fq.pub != NULL || s_fq.weight[0] != NULL || s_fq.weight[1] != NULL ||
            s_fq.doorbell != NULL) {
            all_cleared = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    for (int i = 0; i < 2; i++) atomic_store(&p[i].stop, true);
    for (int w = 0; w < 100 && !(atomic_load(&p[0].done) && atomic_load(&p[1].done)); w++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    test_check(all_freed == 1 && all_cleared == 1, "REQ-WMQ-52_fix_teardown_with_live_producers_always_completes");
    test_check(atomic_load(&p[0].ok) + atomic_load(&p[1].ok) > 0U &&
                   atomic_load(&p[0].refused) + atomic_load(&p[1].refused) > 0U,
               "REQ-WMQ-52_fix_producers_accepted_while_open_refused_while_closed");
    test_check(atomic_load(&p[0].done) && atomic_load(&p[1].done), "REQ-WMQ-52_fix_producers_survive_teardown");
    test_check(heap0 <= heap1 + 64U, "REQ-WMQ-05_fix_init_deinit_cycles_under_load_do_not_leak");
}

static void test_doorbell_not_leaked(void)
{
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int inits = 0;
    for (int i = 0; i < 20; i++) {
        memset(&s_fq, 0, sizeof(s_fq));
        if (mqtt_link_queues_init(&s_fq, 4U, 6U)) inits++;
        if (s_fq.rx != NULL) vQueueDelete(s_fq.rx);
        if (s_fq.pub != NULL) vQueueDelete(s_fq.pub);
        if (s_fq.weight[0] != NULL) vQueueDelete(s_fq.weight[0]);
        if (s_fq.weight[1] != NULL) vQueueDelete(s_fq.weight[1]);
    }
    memset(&s_fq, 0, sizeof(s_fq));
    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    test_check(inits == 20, "REQ-WMQ-05_fix_queue_init_cycles_succeed");
    test_check(heap0 <= heap1 + 64U, "REQ-WMQ-05_fix_queue_only_free_does_not_leak_doorbell");
}

void test_weight_mqtt_fix_run(void)
{
    test_future_stamp();
    test_publish_time_age();
    test_handoff();
    test_deinit_waits_for_consumer();
    test_producers_during_teardown();
    test_doorbell_not_leaked();
}
