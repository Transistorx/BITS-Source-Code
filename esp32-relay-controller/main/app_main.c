#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_status.h"
#include "nvs_config.h"
#include "provisioning.h"
#include "relay_driver.h"
#include "websocket_client.h"
#include "weight_receiver.h"
#include "weight_slots.h"
#include "wifi_manager.h"

#include "dispense_controller.h"
#include "dual_dispense_controller.h"
#include "job_queue.h"
#include "mqtt_link.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "web_api.h"
#include "weight_mqtt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "RELAY_CTRL";

/* mqtt_link -> telemetry_client glue: copies one command message out of the
 * link's RX queue item. The NUL after data[len] is part of the item. */
static bool mqtt_rx_pop_for_telemetry(telemetry_mqtt_rx_t *out, uint32_t timeout_ms)
{
    static mqtt_rx_item_t item;
    if (!mqtt_link_rx_pop(&item, timeout_ms)) return false;
    out->len = item.len;
    out->recv_ms = item.recv_ms;
    out->recv_us = item.recv_us;
    memcpy(out->data, item.data, (size_t)item.len + 1U);
    return true;
}

/* weight_transport=mqtt: drains the separate weight queue and runs the single-scale
 * cache, which feeds the unchanged weight_receiver path. Never touches relays. */
static void weight_mqtt_task(void *arg)
{
    (void)arg;
    static mqtt_wrx_item_t item;
    for (;;) {
        if (mqtt_link_weight_pop(&item, 100U)) {
            (void)weight_mqtt_on_payload(item.data, item.len, item.recv_ms);
        }
    }
}

#define WIFI_CONNECT_TIMEOUT_MS 30000U
/* 10 ms RTOS tick resolution: 15 ms pulses round up to a tick boundary. */
#define SUPERVISOR_PERIOD_MS       10U
#define WS_RETRY_PERIOD_MS      15000U
#define PROV_AP_TIMEOUT_MS     300000U

/* PRE-06: TWDT timeout for the supervisor, comfortably (2x) above the 3 s
 * CONTROL_WATCHDOG_MS so the safety watchdog always acts first. */
#ifndef CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS
#define CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS 3000
#endif
#define SUPERVISOR_TWDT_TIMEOUT_MS 6000U
_Static_assert(SUPERVISOR_TWDT_TIMEOUT_MS >= 2U * CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS,
               "TWDT must outlast the control watchdog");
_Static_assert(SAFETY_WATCHDOG_TASK_PRIORITY > SAFETY_SUPERVISOR_TASK_PRIORITY,
               "safety watchdog must outrank the supervisor");

/* Firmware version. A real constant, not an inline string: it is what the boot
 * log reports and what a field report quotes back. */
#define FIRMWARE_VERSION "relay-controller 7.0-dual-channel"

/* Fallback MUST be 0: when a Kconfig bool is "not set", sdkconfig.h simply
 * omits the symbol, so a fallback of 1 silently re-enabled the boot self-test
 * even though sdkconfig disabled it (confirmed: SELFTEST strings are present
 * in builds whose sdkconfig says "not set"). Only non-Kconfig builds hit this
 * path, and they do not compile app_main anyway. */
#ifndef CONFIG_WEIGHT_DEMO_RELAY_SELFTEST
#define CONFIG_WEIGHT_DEMO_RELAY_SELFTEST 0
#endif

/* A Kconfig choice defines only the SELECTED symbol, so the default branch is
 * the "nothing else matched" case. DISPENSE is the default mode; LEVEL_GROUP
 * keeps the previous all-12-as-one-group bench behaviour available unchanged. */
#if defined(CONFIG_WEIGHT_DEMO_CONTROL_MODE_LEVEL_GROUP)
#define RELAY_USE_DISPENSE 0
#define RELAY_CONTROL_MODE_NAME "LEVEL_GROUP"
#else
#define RELAY_USE_DISPENSE 1
#define RELAY_CONTROL_MODE_NAME "DISPENSE"
#endif

#if CONFIG_WEIGHT_DEMO_RELAY_SELFTEST
/*
 * One-shot boot actuation check, run BEFORE Wi-Fi/NVS/network — the same
 * bare state as a manual GPIO test, executed by this firmware. Drives each
 * channel individually (400 ms ON with command+pad readback logged, then
 * OFF, 100 ms gap; ~6 s total). Discriminates:
 *  (a) every channel energizes in sequence => firmware GPIO drive proven on
 *      hardware; if the later group ON logs cmd=1 pad=1 x12 but nothing
 *      actuates physically, the fault is the simultaneous-load branch (12
 *      SSR input LEDs at once, ~150 mA, sagging the relay-section 5 V rail);
 *  (b) a channel dead here => that channel's GPIO/solder/transistor path,
 *      named exactly in the log;
 *  (c) nothing works here while a bare-metal test did => pin-by-pin
 *      wiring/build mismatch to compare against that test.
 */
static void relay_self_test(void)
{
    ESP_LOGI(TAG, "SELFTEST begin - one channel at a time, ~%u s",
             (unsigned)(RELAY_COUNT / 2U));

    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        esp_err_t err = relay_set(i, true);
        ESP_LOGI(TAG, "SELFTEST Relay%u GPIO%d -> ON (pad=%d)",
                 (unsigned)(i + 1U), relay_gpio_for_index(i),
                 (int)relay_pad_is_energized(i));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SELFTEST Relay%u set failed: %s",
                     (unsigned)(i + 1U), esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(400));

        (void)relay_set(i, false);
        ESP_LOGI(TAG, "SELFTEST Relay%u -> OFF", (unsigned)(i + 1U));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    (void)relay_all_off();
    ESP_LOGI(TAG, "SELFTEST complete - all relays OFF");
}
#endif /* CONFIG_WEIGHT_DEMO_RELAY_SELFTEST */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Hand each accepted single-scale sample to the supervisor, which gives it to the
 * dispense controller. The callback performs only bounded state copies. */
static void on_weight_for_dispense(const weight_msg_t *msg, uint32_t received_ms)
{
    weight_slots_post(msg, received_ms);   /* one scale, one slot: newest sample wins */
}

/* Single-scale configuration (CONTRACT 9.11). Every value is an UNVALIDATED
 * conservative default overridable in NVS. A missing/unreadable key keeps the
 * default; zero is "off" only for start_max_g, elsewhere it falls back to the
 * default (a zero timeout or trip threshold would be a trap, not a setting). */
static uint32_t nvs_u32_or(const char *key, uint32_t dflt)
{
    uint32_t v;
    return nvs_config_get_u32(key, &v) == ESP_OK ? v : dflt;
}

static void load_scale_config(void)
{
    dual_scale_cfg_t cfg;
    dual_dispense_controller_scale_cfg_default(&cfg);
    uint32_t v;
    v = nvs_u32_or(NVS_KEY_RDY_STABLE_S, cfg.ready_stable_ms / 1000U);
    if (v > 0U && v <= 600U) cfg.ready_stable_ms = v * 1000U;
    v = nvs_u32_or(NVS_KEY_RDY_MAX_AGE_S, cfg.ready_max_age_ms / 1000U);
    if (v > 0U && v <= 86400U) cfg.ready_max_age_ms = v * 1000U;
    cfg.reconfirm_same_pump = nvs_u32_or(NVS_KEY_RECONFIRM_SAME, 1U) != 0U;
    cfg.legacy_ready = nvs_u32_or(NVS_KEY_LEGACY_READY, 1U) != 0U;
    v = nvs_u32_or(NVS_KEY_MOVE_TIMEOUT_S, cfg.scale_move_timeout_ms / 1000U);
    if (v > 0U && v <= 86400U) cfg.scale_move_timeout_ms = v * 1000U;
    v = nvs_u32_or(NVS_KEY_START_MAX_G, (uint32_t)cfg.start_max_g);
    if (v <= (uint32_t)WEIGHT_G_MAX) cfg.start_max_g = (int32_t)v;   /* 0 = check disabled */
    v = nvs_u32_or(NVS_KEY_DROP_TRIP_G, (uint32_t)cfg.drop_trip_g);
    if (v > 0U && v <= (uint32_t)WEIGHT_G_MAX) cfg.drop_trip_g = (int32_t)v;
    v = nvs_u32_or(NVS_KEY_NP_LEARN_S, cfg.np_learn_ms / 1000U);
    if (v > 0U && v <= 600U) cfg.np_learn_ms = v * 1000U;
    v = nvs_u32_or(NVS_KEY_NP_ABS_FLOOR_G, (uint32_t)cfg.np_abs_floor_g);
    if (v > 0U && v <= (uint32_t)WEIGHT_G_MAX) cfg.np_abs_floor_g = (int32_t)v;
    v = nvs_u32_or(NVS_KEY_SCALE_RES_G, (uint32_t)cfg.scale_res_g);
    if (v > 0U && v <= 1000U) cfg.scale_res_g = (int32_t)v;
    dual_dispense_controller_set_scale_cfg(&cfg);
    ESP_LOGI(TAG, "scale cfg (UNVALIDATED): stable=%lu ms ready_max_age=%lu ms reconfirm_same=%d "
                  "legacy_ready=%d move_timeout=%lu ms start_max=%ld g drop_trip=%ld g "
                  "np_learn=%lu ms np_floor=%ld g res=%ld g",
             (unsigned long)cfg.ready_stable_ms, (unsigned long)cfg.ready_max_age_ms,
             (int)cfg.reconfirm_same_pump, (int)cfg.legacy_ready,
             (unsigned long)cfg.scale_move_timeout_ms, (long)cfg.start_max_g,
             (long)cfg.drop_trip_g, (unsigned long)cfg.np_learn_ms,
             (long)cfg.np_abs_floor_g, (long)cfg.scale_res_g);
}

/* ---------------------------------------------------------------------------
 * Upstream valve-state publishing
 *
 * The weight sender's dynamic virtual scale needs to know which valves are
 * open, or a simulated dispense would have nothing to integrate. The dispense
 * controller hands its valve state here through a callback rather than doing
 * any networking itself.
 *
 * A queue decouples the two: the control loop must never wait on a socket, and
 * a socket must never stall the valve decision. The sender only needs the
 * LATEST state plus any pending job boundary, so the drain coalesces.
 * ------------------------------------------------------------------------ */
typedef struct {
    bool coarse;
    bool fine;
    bool reset;
} valve_msg_t;

static QueueHandle_t s_valve_tx_q;

#define VALVE_TX_QUEUE_LEN 8U

static void on_valve_change(bool coarse, bool fine, bool reset)
{
    if (s_valve_tx_q == NULL) return;

    const valve_msg_t msg = { coarse, fine, reset };

    /* Non-blocking. On a full queue the OLDEST entry is dropped: the newest
     * valve state is the one that describes the machine right now. A pending
     * job boundary is never dropped — it is folded into the surviving entry. */
    if (xQueueSend(s_valve_tx_q, &msg, 0) != pdTRUE) {
        valve_msg_t dropped;
        if (xQueueReceive(s_valve_tx_q, &dropped, 0) == pdTRUE) {
            valve_msg_t merged = msg;
            merged.reset = merged.reset || dropped.reset;
            (void)xQueueSend(s_valve_tx_q, &merged, 0);
        }
    }
}

static void valve_tx_task(void *arg)
{
    (void)arg;

    while (1) {
        valve_msg_t msg;
        if (xQueueReceive(s_valve_tx_q, &msg, portMAX_DELAY) != pdTRUE) continue;

        /* Coalesce a burst down to the newest state, preserving any reset. */
        valve_msg_t newer;
        while (xQueueReceive(s_valve_tx_q, &newer, 0) == pdTRUE) {
            msg.reset = msg.reset || newer.reset;
            msg.coarse = newer.coarse;
            msg.fine = newer.fine;
        }

        char frame[96];
        int n = snprintf(frame, sizeof(frame),
                         "{\"type\":\"relay_state\",\"coarse\":%s,\"fine\":%s,"
                         "\"reset\":%s}",
                         msg.coarse ? "true" : "false",
                         msg.fine ? "true" : "false",
                         msg.reset ? "true" : "false");
        if (n <= 0 || (size_t)n >= sizeof(frame)) continue;

        if (!websocket_client_send_text(frame, 500U)) {
            /* Only worth saying once per drop burst: the sender not being
             * connected is normal before its WebSocket comes up, and the
             * dynamic scale simply stays empty until the link is live. */
            ESP_LOGD(TAG, "valve state not published (sender link down)");
        }
    }
}

/* ---------------------------------------------------------------------------
 * Supervisor
 * ------------------------------------------------------------------------ */

static void on_wifi_lost(void)
{
    uint32_t t = now_ms();
    weight_receiver_on_link_lost(t);
    safety_manager_raise(SAFETY_WEIGHT_LINK_LOST, t);
}

/* Mirrors the receiver's link verdict into the safety latch so a link fault is
 * visible in one place regardless of which mode is running. Both faults are
 * transient: a fresh valid weight clears them, so a bench run recovers by
 * itself instead of needing an operator. */
static void safety_watch_link(uint32_t t)
{
    switch (weight_receiver_link_state()) {
    case WEIGHT_LINK_LOST:
        safety_manager_raise(SAFETY_WEIGHT_LINK_LOST, t);
        break;
    case WEIGHT_LINK_TIMEOUT:
        /* A manually held relay is exempt from the stale-weight fault: the
         * operator is its feedback loop. Link loss is NOT exempt (above). */
        if (weight_receiver_manual_channel() == 0U) {
            safety_manager_raise(SAFETY_WEIGHT_STALE, t);
        }
        break;
    default:
        break;
    }
}

/* Enforces WEIGHT_MESSAGE_TIMEOUT_MS, advances the dispensing state machine and
 * runs the control watchdog. Kept off the WebSocket event handler so no
 * callback ever blocks, and kept in ONE task so the three steps see a
 * consistent view of the same instant. */
static void supervisor_task(void *arg)
{
    (void)arg;

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK)
        ESP_LOGE(TAG, "supervisor not subscribed to TWDT: %s", esp_err_to_name(wdt_err));

    while (1) {
        (void)esp_task_wdt_reset();
        uint32_t t = now_ms();

        weight_slot_sample_t sample;
        while (weight_slots_take(&sample)) {
            dual_dispense_controller_on_weight(&sample.msg,sample.acquired_ms);
            telemetry_client_on_weight(&sample.msg,sample.acquired_ms);
        }

        weight_receiver_tick(t);   /* hard failsafe: all relays OFF when stale */
        safety_watch_link(t);

        if (RELAY_USE_DISPENSE) {
            dual_dispense_controller_tick(t);
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
}

/* Independent of the controller task: a blocked supervisor cannot declare
 * its own heartbeat healthy. This task never takes the controller mutex. */
static void safety_watchdog_task(void *arg)
{
    (void)arg;
    for(;;){
        safety_manager_tick(now_ms());
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

/*
 * BOOT button (GPIO0) monitor — two thresholds on the existing hardware, so
 * provisioning reset needs no new button or jumper:
 *   3 s  : reboot into the provisioning portal (credentials are overwritten)
 *   10 s : PROVISIONING RESET — erase the stored credentials from NVS first,
 *          then reboot into the portal so the device comes up unprovisioned
 *          even if the user walks away without submitting the form.
 * The erase is the discriminating action: without it the old credentials stay
 * in NVS and the device silently rejoins the previous network.
 */
static void button_task(void *arg)
{
    (void)arg;

    int press_count = 0;
    bool erase_done = false;
    while (1) {
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            press_count++;
            if (press_count >= 100 && !erase_done) { /* 10 seconds */
                erase_done = true;
                ESP_LOGW(TAG, "Provisioning reset: erasing stored credentials");
                esp_err_t err = nvs_config_erase_all();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "credential erase failed: %s - forcing portal only",
                             esp_err_to_name(err));
                }
                nvs_config_set_u32(NVS_KEY_FORCE_PROV, 1);
                esp_restart();
            }
            if (press_count >= 30) { /* 3 seconds */
                ESP_LOGI(TAG, "Provisioning button held - rebooting into provisioning mode");
                nvs_config_set_u32(NVS_KEY_FORCE_PROV, 1);
                esp_restart();
            }
        } else {
            press_count = 0;
            erase_done = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Phase 1: resolve the weight server and start the WebSocket client, retrying
 * every 15 s until the start succeeds — the server (or Wi-Fi, right after
 * provisioning) may not be up yet. The sender holds a static lease (primary
 * target is its configured IP, not mDNS), so there is no stale-address
 * failure mode and no re-resolution logic.
 * Phase 2: stay alive as a slow link supervisor. The client's own bounded
 * auto-reconnect (2 s backoff + 5 s ping / 10 s pong timeout) does the work;
 * this loop only guarantees a permanently-failing link is NEVER silent at
 * INFO: one rate-limited warning every 30 s while disconnected, naming the
 * target. Cheap (one bool read), no client re-init. The device never reboots
 * because the peer is temporarily unavailable. */
#define WS_SUPERVISOR_PERIOD_MS 30000U

static void ws_connect_task(void *arg)
{
    (void)arg;

    bool announced = false;
    while (websocket_client_start() != ESP_OK) {
        if (!announced) {
            ESP_LOGW(TAG, "Weight server not reachable yet; retrying every %u s",
                     (unsigned)(WS_RETRY_PERIOD_MS / 1000U));
            announced = true;
        }
        vTaskDelay(pdMS_TO_TICKS(WS_RETRY_PERIOD_MS));
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WS_SUPERVISOR_PERIOD_MS));
        if (!websocket_client_is_connected()) {
            ESP_LOGW(TAG, "still trying %s (client auto-reconnect active)",
                     websocket_client_target());
        }
    }
}

void app_main(void)
{
    /* Build stamp: a stale flash is the most common cause of "code looks
     * right, device behaves old" — this line makes a mismatch instant. */
    ESP_LOGI(TAG, "boot | fw %s " __DATE__ " " __TIME__, FIRMWARE_VERSION);
    ESP_LOGI(TAG, "control mode: %s", RELAY_CONTROL_MODE_NAME);

    /* Safety first: every relay must be OFF before Wi-Fi or WebSocket exist.
     * Nothing below this point can energize a relay on its own — except the
     * explicit, Kconfig-gated one-shot self-test below, which runs before any
     * network exists and ends with all channels OFF. */
    ESP_ERROR_CHECK(relay_init_all());

#if CONFIG_WEIGHT_DEMO_RELAY_SELFTEST
    relay_self_test();
#endif

    /* Stage B (Kconfig-gated, default n): user-attended expander mapping /
     * polarity test. Runs before any network exists; MAINS DISCONNECTED is a
     * hard precondition (announced in the 5 s countdown). relay_init_all()
     * above already ran the read-only discovery + boot fail-safe. */
#if CONFIG_WEIGHT_DEMO_I2C_MAP_TEST
    if (relay_backend_is_i2c()) {
        relay_backend_run_map_test();
    } else {
        ESP_LOGW(TAG, "MAPTEST requested but the confirmed expander backend is "
                      "not active (detection or chip gate failed) - skipped");
    }
#endif

    /* C5-1 (Kconfig-gated, default n): write+readback discriminator, run
     * BEFORE Wi-Fi with a mains-disconnected countdown — the automatic
     * actuation path stays gated until verification completes. */
#if CONFIG_WEIGHT_DEMO_I2C_PCF_TEST
    if (relay_backend_is_i2c()) {
        relay_backend_run_pcf_test();
    } else {
        ESP_LOGW(TAG, "PCF_TEST requested but the confirmed expander backend is "
                      "not active (detection or chip gate failed) - skipped");
    }
#endif

#if CONFIG_WEIGHT_DEMO_I2C_ACT_TEST
    if (relay_backend_is_i2c()) {
        relay_backend_run_act_test();
    } else {
        ESP_LOGW(TAG, "ACT_TEST requested but the expander backend is not active "
                      "(detection or chip gate failed) - skipped");
    }
#endif

    ESP_ERROR_CHECK(weight_receiver_init());
    ESP_ERROR_CHECK(safety_manager_init());
    /* M5: relays are already OFF (relay_init_all above). A watchdog/panic reboot
     * latches CONTROL_FAILURE before any job can run. */
    (void)safety_manager_note_boot_reset(esp_reset_reason(), now_ms());
    ESP_ERROR_CHECK(job_queue_init());

    /* The dispense controller registers as the weight sink, replacing the
     * legacy group decision only. The receiver keeps parsing, validation and
     * the stale/link failsafe either way. */
    if (RELAY_USE_DISPENSE) {
        ESP_ERROR_CHECK(dual_dispense_controller_init());
        weight_receiver_set_actuation_handler(on_weight_for_dispense);
    } else {
        ESP_LOGW(TAG, "LEVEL_GROUP mode: all 12 relays follow the %ld g trigger; "
                      "the dispense queue and state machine are disabled",
                 (long)WEIGHT_TRIGGER_G);
    }

    /* NVS + device ID — same provisioning store as the UART weighing-scale
     * gateway. Wi-Fi credentials are written by the captive portal, not by
     * menuconfig. */
    ESP_ERROR_CHECK(nvs_config_init());
    ESP_ERROR_CHECK(nvs_config_load_defaults());
    ESP_ERROR_CHECK(nvs_config_ensure_device_id());

    /* Weight source: exactly one of WebSocket (default) or MQTT feeds the
     * receiver; never merged, no automatic failover. A selected-but-unusable
     * MQTT source (peer id missing/invalid) feeds nothing, so weight goes stale
     * and the relays stay OFF. */
    char wxport[8] = "ws";
    char peer_id[MQTT_LINK_DEVICE_ID_MAX + 1U] = "";
    if (nvs_config_get_str(NVS_KEY_WEIGHT_XPORT, wxport, sizeof(wxport)) != ESP_OK) {
        strlcpy(wxport, "ws", sizeof(wxport));
    }
    if (nvs_config_get_str(NVS_KEY_PEER_SENDER_ID, peer_id, sizeof(peer_id)) != ESP_OK) {
        peer_id[0] = '\0';
    }
    weight_mqtt_init();
    {
        /* Only this scale_id is accepted from weight/ctl; a mismatch is rejected and
         * counted. An unreadable or invalid value accepts NO weight (fail safe). */
        char scale_id[24] = WEIGHT_MQTT_SCALE_ID_DEFAULT;
        if (nvs_config_get_str(NVS_KEY_SCALE_ID, scale_id, sizeof(scale_id)) != ESP_OK) {
            strlcpy(scale_id, WEIGHT_MQTT_SCALE_ID_DEFAULT, sizeof(scale_id));
        }
        if (!weight_mqtt_set_expected_scale_id(scale_id)) {
            ESP_LOGE(TAG, "NVS scale_id '%s' is invalid - NO weight will be accepted", scale_id);
        }
    }
    load_scale_config();
    const weight_sel_t wsel = weight_mqtt_select(wxport, peer_id);
    if (wsel != WEIGHT_SEL_WS) {
        websocket_client_set_feed_enabled(false);
        ESP_LOGW(TAG, "weight transport: MQTT (%s); WebSocket weight feed disabled",
                 wsel == WEIGHT_SEL_MQTT ? "peer set" : "peer_sender_id missing/invalid - NO weight");
    }

    /* Status LED: DISABLED on this board (CONFIG_LED_STATUS_GPIO=-1). GPIO2 is
     * AO12 (Relay 12) and relay_driver must be its single owner, so the LED
     * service relinquishes the pin and provisioning/connect hints are log-only
     * here. led_status_set() calls elsewhere compile to no-ops. */
    esp_err_t led_err = led_status_init();
    if (led_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Status LED disabled - state changes are log-only");
    } else {
        ESP_ERROR_CHECK(led_err);
    }

    /* BOOT button (GPIO0): hold 3 s to reboot into the provisioning portal. */
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);

    uint32_t force_prov = 0;
    nvs_config_get_u32(NVS_KEY_FORCE_PROV, &force_prov);
    if (force_prov == 1) {
        nvs_config_set_u32(NVS_KEY_FORCE_PROV, 0); /* clear immediately */

        /* BITS-Relay-XXXX, XXXX from the unique device id. */
        char dev_id[32] = {0};
        (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, dev_id, sizeof(dev_id));

        char ap_name[64];
        provisioning_build_ap_name("BITS-Relay", dev_id, ap_name, sizeof(ap_name));

        ESP_LOGI(TAG, "Entering forced provisioning mode...");
        provisioning_run(ap_name, PROV_AP_TIMEOUT_MS); /* restarts, does not return */
    }

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    /* Register the failsafe before the radio can emit any event, so no
     * disconnect — however early — can miss the relay-off call. */
    wifi_manager_set_link_lost_callback(on_wifi_lost);
    ESP_ERROR_CHECK(wifi_manager_start());

    if (wifi_manager_is_started() &&
        !wifi_manager_wait_connected(WIFI_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Wi-Fi connect timeout; still retrying in the background");
    }

    /* Operator surface. Started before the WebSocket client so the controller
     * is reachable even when the sender is not: a bench operator can queue a
     * job and watch it wait in WAIT_FOR_SCALE. */
    esp_err_t web_err = web_api_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "Web API unavailable: %s", esp_err_to_name(web_err));
    }
    /* Before any task (weight_mqtt included) can post a weight sample. */
    ESP_ERROR_CHECK(weight_slots_init() ? ESP_OK : ESP_ERR_NO_MEM);
    if (RELAY_USE_DISPENSE) {
        /* MQTT is an additional transport for telemetry and operator commands;
         * HTTP stays as the fallback. The ops are registered first so the
         * telemetry tasks (incl. mqtt_cmd) see them from their first loop. */
        static const telemetry_mqtt_ops_t mqtt_ops = {
            .connected = mqtt_link_connected,
            .publish = mqtt_link_publish,
            .rx_pop = mqtt_rx_pop_for_telemetry,
            .run_ack_pop = mqtt_link_run_ack_pop,
        };
        telemetry_client_set_mqtt_ops(&mqtt_ops);
        if (wsel == WEIGHT_SEL_MQTT && mqtt_link_set_weight_peer(peer_id)) {
            weight_mqtt_set_enabled(true);
            xTaskCreate(weight_mqtt_task, "weight_mqtt", 4096, NULL, 4, NULL);
        }
        esp_err_t telemetry_err = telemetry_client_start();
        if (telemetry_err != ESP_OK) ESP_LOGE(TAG, "Telemetry client unavailable: %s", esp_err_to_name(telemetry_err));
        else if (telemetry_client_device_id()[0] != '\0') {
            /* Same device id as the HTTP payloads, so topic and body agree. */
            if (telemetry_client_run_mqtt_enabled()) mqtt_link_set_run_ack(true);
            esp_err_t mqtt_err = mqtt_link_start(telemetry_client_device_id());
            if (mqtt_err != ESP_OK) ESP_LOGW(TAG, "MQTT link not started (%s); HTTP only", esp_err_to_name(mqtt_err));
        }
    }

    s_valve_tx_q = xQueueCreate(VALVE_TX_QUEUE_LEN, sizeof(valve_msg_t));
    if (s_valve_tx_q != NULL) {
        xTaskCreate(valve_tx_task, "valve_tx", 3072, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "valve-state publish queue unavailable - the simulated "
                      "scale will not track the valves");
    }

    /* PRE-06: a hard hang of the supervisor must reset the chip (relays are
     * forced OFF at boot by safety_manager_init). The supervisor subscribes to
     * the TWDT below; panic-on-timeout is enabled here instead of in sdkconfig. */
    {
        const esp_task_wdt_config_t twdt = {
            .timeout_ms = SUPERVISOR_TWDT_TIMEOUT_MS,
            .idle_core_mask = (1U << 0) | (1U << 1), /* unchanged from sdkconfig */
            .trigger_panic = true,
        };
        esp_err_t twdt_err = esp_task_wdt_reconfigure(&twdt);
        if (twdt_err != ESP_OK)
            ESP_LOGE(TAG, "TWDT reconfigure failed: %s", esp_err_to_name(twdt_err));
    }
    xTaskCreate(supervisor_task, "relay_supervisor", 4096, NULL,
                SAFETY_SUPERVISOR_TASK_PRIORITY, NULL);
    xTaskCreate(safety_watchdog_task, "safety_watchdog", 3072, NULL,
                SAFETY_WATCHDOG_TASK_PRIORITY, NULL);


    /* The relay stays OFF until a NEW valid weight message arrives. */
    xTaskCreate(ws_connect_task, "ws_connect", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Relay controller initialised");
}
