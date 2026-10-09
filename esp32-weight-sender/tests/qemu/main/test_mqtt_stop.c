#include "test_mqtt_stop.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_link_core.h"
#include "test_harness.h"

#define DEV "bits-a4cf12ab34cd"

static mqtt_item_t s_scratch;
static int s_flushed;
static int s_flushed_qos1;

static void count_cb(const mqtt_item_t *item, void *ctx)
{
    (void)ctx;
    s_flushed++;
    if (item->qos > 0U) s_flushed_qos1++;
}

/* Stand-in for mqtt_pub_task: loops while the link is RUNNING, then reports. */
typedef struct {
    mqtt_link_lc_t *lc;
    mqtt_link_queues_t *q;
    atomic_bool done;
} pub_ctx_t;

static void fake_pub_task(void *arg)
{
    pub_ctx_t *c = (pub_ctx_t *)arg;
    static mqtt_item_t item;
    while (mqtt_link_lc_running(c->lc)) {
        (void)mqtt_link_queue_next(c->q, &item, 20U, 0U, 1000U);
    }
    atomic_store(&c->done, true);
    vTaskDelete(NULL);
}

void test_mqtt_stop_run(void)
{
    /* ---- graceful payload ---- */
    mqtt_link_presence_t p;
    test_check(mqtt_link_presence_plan(&p, DEV), "stop_presence_plan_ok");
    test_check(p.graceful_payload != NULL &&
                   strcmp(p.graceful_payload, "{\"online\":false,\"graceful\":true}") == 0,
               "stop_graceful_payload_is_offline_graceful");
    test_check(strcmp(p.lwt_payload, "{\"online\":false}") == 0, "stop_lwt_payload_unchanged");
    test_check(p.qos == 1 && p.retain, "stop_graceful_is_qos1_retained");
    test_check(strcmp(p.lwt_topic, "cas/" DEV "/status") == 0, "stop_graceful_topic_is_status");

    /* ---- lifecycle state machine ---- */
    mqtt_link_lc_t lc;
    mqtt_link_lc_init(&lc);
    test_check(mqtt_link_lc_state(&lc) == MQTT_LC_IDLE, "stop_lc_starts_idle");
    test_check(!mqtt_link_lc_running(&lc), "stop_lc_idle_not_running");
    test_check(!mqtt_link_lc_begin_stop(&lc), "stop_before_start_is_noop");
    test_check(mqtt_link_lc_start(&lc), "stop_lc_start_ok");
    test_check(!mqtt_link_lc_start(&lc), "stop_lc_double_start_refused");
    test_check(mqtt_link_lc_running(&lc), "stop_lc_running_after_start");
    test_check(mqtt_link_lc_begin_stop(&lc), "stop_lc_first_stop_wins");
    test_check(mqtt_link_lc_state(&lc) == MQTT_LC_STOPPING, "stop_lc_stopping");
    test_check(!mqtt_link_lc_running(&lc), "stop_lc_stopping_not_running");
    test_check(!mqtt_link_lc_begin_stop(&lc), "stop_lc_second_stop_is_idempotent");
    test_check(!mqtt_link_lc_start(&lc), "stop_lc_no_start_while_stopping");
    mqtt_link_lc_end_stop(&lc);
    test_check(mqtt_link_lc_state(&lc) == MQTT_LC_IDLE, "stop_lc_idle_after_stop");
    test_check(!mqtt_link_lc_begin_stop(&lc), "stop_lc_stop_after_stop_is_noop");
    test_check(mqtt_link_lc_start(&lc), "stop_lc_restart_after_stop_works");
    test_check(mqtt_link_lc_begin_stop(&lc), "stop_lc_second_cycle_stops");
    mqtt_link_lc_end_stop(&lc);

    /* ---- queue: bounded flush, deinit, no publish after stop ---- */
    mqtt_link_queues_t q;
    test_check(mqtt_link_queues_init(&q, MQTT_LINK_RX_DEPTH, MQTT_LINK_PUB_DEPTH), "stop_queue_init");
    for (int i = 0; i < 6; i++) {
        (void)mqtt_link_queue_push_pub(&q, "cas/x/commands/ack", "{}", (i % 2) ? 1 : 0, false);
    }
    test_check(mqtt_link_queue_pending(&q) == 6U, "stop_queue_pending_counts");
    s_flushed = 0;
    s_flushed_qos1 = 0;
    test_check(mqtt_link_queue_flush(&q, &s_scratch, 4U, count_cb, NULL) == 4U && s_flushed == 4,
               "stop_flush_is_bounded_by_max");
    test_check(mqtt_link_queue_pending(&q) == 2U, "stop_flush_leaves_remainder");
    test_check(mqtt_link_queue_flush(&q, &s_scratch, 4U, count_cb, NULL) == 2U && s_flushed == 6,
               "stop_flush_drains_when_fewer_than_max");
    test_check(s_flushed_qos1 == 3, "stop_flush_hands_over_qos_intact");
    test_check(mqtt_link_queue_flush(&q, &s_scratch, 4U, count_cb, NULL) == 0U,
               "stop_flush_empty_is_zero");
    test_check(mqtt_link_queue_flush(NULL, &s_scratch, 4U, count_cb, NULL) == 0U &&
                   mqtt_link_queue_flush(&q, NULL, 4U, count_cb, NULL) == 0U &&
                   mqtt_link_queue_flush(&q, &s_scratch, 4U, NULL, NULL) == 0U,
               "stop_flush_rejects_null");

    mqtt_link_queues_deinit(&q);
    test_check(q.rx == NULL && q.pub == NULL && q.weight[0] == NULL && q.weight[1] == NULL &&
                   q.doorbell == NULL,
               "stop_queue_deinit_clears_handle");
    test_check(mqtt_link_queue_push_pub(&q, "cas/x/status", "{}", 1, true) == MQTT_ENQ_INVALID,
               "stop_no_publish_after_deinit");
    test_check(mqtt_link_queue_push_rx(&q, "t", 1U, "d", 1U, 1U, false, 0U) == MQTT_ENQ_INVALID,
               "stop_no_rx_after_deinit");
    test_check(mqtt_link_queue_pending(&q) == 0U, "stop_pending_zero_after_deinit");
    mqtt_link_queues_deinit(&q);
    mqtt_link_queues_deinit(NULL);
    test_check(true, "stop_queue_deinit_is_idempotent");

    /* ---- pub task exits cleanly; clean restart; no leaks ---- */
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (int cycle = 0; cycle < 3; cycle++) {
        static pub_ctx_t ctx;
        mqtt_link_lc_init(&lc);
        ctx.lc = &lc;
        ctx.q = &q;
        atomic_store(&ctx.done, false);
        bool ok = mqtt_link_lc_start(&lc) && mqtt_link_queues_init(&q, MQTT_LINK_RX_DEPTH, MQTT_LINK_PUB_DEPTH) &&
                  xTaskCreate(fake_pub_task, "fake_pub", 3072, &ctx, 2, NULL) == pdPASS;
        test_check(ok, "stop_cycle_start_ok");
        vTaskDelay(pdMS_TO_TICKS(50));
        test_check(!atomic_load(&ctx.done), "stop_pub_task_runs_until_stop");
        (void)mqtt_link_lc_begin_stop(&lc);
        int waited = 0;
        while (!atomic_load(&ctx.done) && waited < 500) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        test_check(atomic_load(&ctx.done) && waited < 200, "stop_pub_task_exits_promptly");
        vTaskDelay(pdMS_TO_TICKS(30)); /* let the idle task reap the deleted task */
        mqtt_link_queues_deinit(&q);
        mqtt_link_lc_end_stop(&lc);
        test_check(mqtt_link_queue_push_pub(&q, "cas/x/status", "{}", 1, true) == MQTT_ENQ_INVALID,
                   "stop_cycle_no_publish_after_stop");
    }
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    test_check(heap_before <= heap_after + 64U, "stop_restart_cycles_do_not_leak");
}
