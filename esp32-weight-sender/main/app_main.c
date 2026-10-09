#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_status.h"
#include "log_util.h"
#include "mqtt_link.h"
#include "mqtt_link_core.h"
#include "nvs_config.h"
#include "provisioning.h"
#include "remote_cmd.h"
#include "sdkconfig.h"
#include "websocket_server.h"
#include "weight_source.h"
#include "wifi_manager.h"

#include "board_pins.h"
#include "cas_ci2001_parser.h"
#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
#include "asuki_k1_parser.h"
#include "asuki_parser.h"
#include "cas_parser.h"
#include "flintec_parser.h"
#endif
#include "scale_events.h"
#include "scale_manager.h"
#include "scale_reader.h"
#include "scale_transport_rs232.h"

#include <stdio.h>

static const char *TAG = "SYS";
static const char *TAG_NET = "NET";
static const char *TAG_MQTT = "MQTT";
static const char *TAG_CMD = "CMD";
static const char *TAG_RS485 = "RS485";
static const char *TAG_QUEUE = "QUEUE";

/* UART sampling is independent; the newest sample is polled every 20 ms and
 * sends are deduped by sample sequence, so the rate never exceeds the scale's. */
#define TRANSMIT_PERIOD_MS       20U
#define WIFI_CONNECT_TIMEOUT_MS 30000U
#define SCALE_EVENT_QUEUE_LEN   20U

/* Firmware version. A real constant, not an inline string: it is what the boot
 * log reports and what a field report quotes back. Defined before the status
 * task so the report payload can carry it. */
#define FIRMWARE_VERSION "weight-sender 7.0-cas-physical"

/* Transport label for the MQTT weight topics; the WS payload keeps its legacy
 * "CAS_RS232" string for compatibility. */
#if BOARD_VARIANT_IS_RS485
#define MQTT_WEIGHT_SOURCE "CAS_RS485"
#else
#define MQTT_WEIGHT_SOURCE "CAS_RS232"
#endif

/*
 * This board reports ITS OWN health. It is a first-class device with its own
 * device_id (derived from its own WiFi STA MAC), so the web UI's "Weight
 * Sender" indicator is driven by this status â€” not by another board's opinion
 * of whether it can reach us. Role is explicit so the server can tell the two
 * ESP32s apart regardless of arrival order.
 *
 * Published over MQTT (cas/{device}/telemetry/status, QoS 0, not retained) as
 * the flat status object; the topic carries the device id. The publish only
 * enqueues for the mqtt_pub task, so a dead broker cannot stall this task or
 * the weight path. Presence (online:true) is the MQTT birth message, sent on
 * connect by mqtt_link, never from here.
 */
static void server_health_task(void *arg)
{
    (void)arg;
    char body[400];

    for (;;) {
        uint32_t now_ms = weight_source_now_ms();
        cas_link_state_t cas = weight_source_cas_link_state();
        uint32_t cas_age = weight_source_have_valid_cas()
                               ? (now_ms - weight_source_last_valid_cas_ms())
                               : UINT32_MAX;

        int n = snprintf(body, sizeof(body),
                         "{\"role\":\"weight_sender\","
                         "\"boot_id\":\"%s\","
                         "\"firmware\":\"%s\","
                         "\"cas_link\":\"%s\","
                         "\"cas_seq\":%lu,"
                         "\"cas_age_ms\":%lu,"
                         "\"ws_clients\":%d,"
                         "\"uptime_ms\":%lu}",
                         mqtt_link_boot_id(),
                         FIRMWARE_VERSION,
                         cas_link_state_name(cas),
                         (unsigned long)weight_source_cas_sequence(),
                         (unsigned long)cas_age,
                         websocket_server_has_client() ? 1 : 0,
                         (unsigned long)now_ms);
        if (n <= 0 || (size_t)n >= sizeof(body)) {
            ESP_LOGE(TAG, "status body overflow");
        } else {
            (void)mqtt_link_publish(MQTT_SUFFIX_STATUS, body, 0, false);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MQTT_LINK_STATUS_PERIOD_MS));
    }
}

static void server_health_start(void)
{
    mqtt_link_set_identity(FIRMWARE_VERSION, "weight_sender");
    esp_err_t err = remote_cmd_start();
    if (err == ESP_OK) err = mqtt_link_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG_MQTT, "link not started: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(server_health_task, "dev_status", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start device status task");
    }
}

#define PROV_AP_TIMEOUT_MS      300000U

static QueueHandle_t s_scale_events;

/* PRODUCTION: exactly one protocol, the CAS CI-2001 / CI-150A parser. No
 * auto-detect, no generic ASCII fallback, no other parser compiled in: a
 * dispensing weight source must not accept a line just because some parser
 * happens to like it. Auto-detect is an explicit legacy Kconfig option
 * (SCALE_PROTOCOL_AUTODETECT_LEGACY) and is off by default.
 * Non-const elements: scale_reader_config_t takes `const scale_protocol_t **`. */
#ifdef CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY
static const scale_protocol_t *k_protocols[] = {
    &asuki_protocol,
    &asuki_k1_protocol,
    &flintec_protocol,
    &cas_protocol,
    &cas_ci2001_protocol,
};
#define SCALE_AUTODETECT_ENABLED true
#else
static const scale_protocol_t *k_protocols[] = {
    &cas_ci2001_protocol,
};
#define SCALE_AUTODETECT_ENABLED false
#endif

#ifdef CONFIG_SCALE_CI2001_STRICT_PAD
#define SCALE_STRICT_PAD true
#define SCALE_PAD_VALUE ((uint8_t)CONFIG_SCALE_CI2001_PAD_VALUE)
#else
#define SCALE_STRICT_PAD false
#define SCALE_PAD_VALUE ((uint8_t)0)
#endif
#ifdef CONFIG_SCALE_ALLOW_GENERIC_FALLBACK
#define SCALE_GENERIC_ENABLED true
#else
#define SCALE_GENERIC_ENABLED false
#endif

/*
 * BOOT button (GPIO0) monitor â€” two thresholds on the existing hardware, so
 * provisioning reset needs no new button or jumper:
 *   3 s  : reboot into the provisioning portal (credentials are overwritten)
 *   10 s : PROVISIONING RESET â€” erase the stored credentials from NVS first,
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

/*
 * Publishes ONLY genuine CAS RS232 samples. Generation is NEVER gated on
 * client presence: the CAS scale streams on its own, and the per-second INFO
 * lines make the runtime path observable whether or not anybody is listening.
 * The sequence counter advances ONLY when a broadcast actually succeeds, so
 * a reconnecting peer never sees a bogus "missed N" gap.
 *
 * There is no simulator in this task. When the CAS scale produces no fresh
 * frame the task publishes NOTHING and the CAS link state is logged as
 * WAITING / STALE / OFFLINE. A substituted weight would hide a real fault
 * and could drive a dispense.
 */
static void transmit_task(void *arg)
{
    (void)arg;

    char json[320];
    bool had_client = false;
    int last_logged_cas_seq = -1;
    uint32_t last_queued_cas_seq = 0U;
    log_ratelimit_t build_rl;
    log_ratelimit_init(&build_rl);
    TickType_t wake = xTaskGetTickCount();

    while (1) {
        uint32_t now_ms = weight_source_now_ms();

        bool has_client = websocket_server_has_client();
        if (has_client != had_client) {
            /* Connect is already logged by websocket_server; only the loss is
             * worth a line here. */
            if (!has_client) {
                ESP_LOGW(TAG_NET, "no WebSocket client - weight not transmitted");
            }
            had_client = has_client;
        }

        /* Provenance state machine: WAITING -> ONLINE -> STALE -> OFFLINE.
         * ONLINE is reached only after a valid frame has been parsed. */
        weight_source_log_cas_status(now_ms);

        weight_sample_t sample;
        weight_source_result_t whence = weight_source_get(&sample);

        if (whence != WEIGHT_SOURCE_RESULT_REAL) {
            /* No fresh real CAS data. Publishing nothing is deliberate: the
             * relay controller's stale-weight timeout is the correct, safe
             * response to a scale that is not reporting. There is NO fallback
             * weight of any kind. The SCALE link-state transition is logged
             * once by weight_source_log_cas_status(). */
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(TRANSMIT_PERIOD_MS));
            continue;
        }

        /* MQTT weight copy: independent of WS client presence, valid real
         * frames only, deduped by cas_seq inside. Zero-timeout enqueue, so a
         * dead broker cannot slow this loop. */
        if (sample.channel == 1U || sample.channel == 2U) {
            mqtt_link_weight_t mw = {
                .uptime_ms = now_ms,
                .src_uart = sample.channel,
                .weight_g = sample.weight_g,
                .stable = sample.stable,
                .age_ms = sample.age_ms,
                .cas_seq = sample.sequence,
                .source = MQTT_WEIGHT_SOURCE,
            };
            mqtt_link_weight_frame(&mw, now_ms);
        }

        /* Repeated polls are not new scale samples. Do not renew receiver
         * freshness with the same physical acquisition. */
        if (sample.sequence == last_queued_cas_seq) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(TRANSMIT_PERIOD_MS));
            continue;
        }

        /* Per-sample provenance line is DEBUG only and rate-limited. */
        if ((int)sample.sequence != last_logged_cas_seq) {
            weight_source_log_cas_sample(&sample, weight_source_peek_sequence());
            last_logged_cas_seq = (int)sample.sequence;
        }

        uint32_t sequence = weight_source_peek_sequence();
        size_t len = weight_build_json(json, sizeof(json), &sample, sequence);
        if (len == 0U) {
            uint32_t folded;
            if (log_ratelimit_event(&build_rl, now_ms, 30000U, &folded)) {
                ESP_LOGE(TAG_NET, "weight message build failed (x%lu)", (unsigned long)folded);
            }
        } else if (has_client) {
            /* broadcast() == true means the frame was QUEUED for send in the
             * httpd task (the IDF-sanctioned context for
             * httpd_ws_send_frame_async), not yet confirmed on the wire; a
             * failed actual send surfaces as its own WARN with the esp_err
             * code and tears the socket down so the peer reconnects. The
             * sequence advances only when the frame was handed to httpd. */
            if (websocket_server_broadcast(json)) {
                weight_source_advance_sequence();
                last_queued_cas_seq = sample.sequence;
                ESP_LOGD(TAG_NET, "TX weight=%dg seq=%u age=%ums", sample.weight_g,
                         (unsigned)sequence, (unsigned)sample.age_ms);
            } else {
                ESP_LOGD(TAG_NET, "TX weight=%dg seq=%u not queued", sample.weight_g,
                         (unsigned)sequence);
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TRANSMIT_PERIOD_MS));
    }
}

/*
 * Diagnostics task: the ONLY place that turns counters into serial lines.
 * Priority 1 (below every working task), never blocks on a lock, and reads
 * counters through lock-free / snapshot getters, so the UART RX path, the
 * esp-mqtt callback and the weight path never wait on serial output.
 *   - one health line every CONFIG_HEALTH_SUMMARY_PERIOD_S (0 = off)
 *   - RS-485 fault counters and event-queue drops, reported when they change,
 *     at most once per ERR_LOG_INTERVAL_MS with a count
 */
#define DIAG_TICK_MS         1000U
#define ERR_LOG_INTERVAL_MS 30000U

static uint32_t uart_fault_total(const scale_reader_diagnostics_t *d)
{
    return d->fifo_overflows + d->driver_buffer_overflows + d->break_events +
           d->parity_errors + d->framing_errors + d->parser_overflows +
           d->uart_event_queue_saturations + d->uart_read_errors;
}

/* One line per link transition, with the counters at that instant and the
 * reason the reader changed state. Transitions are at least ~3 s apart per
 * channel (2 frames to go online, 3 s of silence to go offline), so this is
 * bounded without a limiter. */
static void log_link_transitions(uint8_t ch, const scale_reader_diagnostics_t *d,
                                 uint32_t *seen)
{
    if (d->link_transitions == *seen) return;
    uint32_t missed = d->link_transitions - *seen - 1U;
    *seen = d->link_transitions;
    const board_scale_link_t *link = board_pinmap_get((board_channel_t)ch);
    log_link_transition_t t = {
        .ch = (uint8_t)(ch + 1U),
        .uart = link != NULL ? link->uart_port : -1,
        .selected = ((board_channel_t)ch == board_pinmap_active_channel()),
        .to_online = d->link_online,
        .reason = scale_link_reason_name(d->link_reason),
        .bytes = d->link_trans_bytes,
        .valid = d->link_trans_valid,
        .rejected = d->link_trans_rejected,
    };
    char line[200];
    if (log_fmt_link_transition(line, sizeof(line), &t) == 0U) return;
    if (d->link_online) {
        ESP_LOGI(TAG_RS485, "%s (missed=%lu)", line, (unsigned long)missed);
    } else {
        ESP_LOGW(TAG_RS485, "%s (missed=%lu)", line, (unsigned long)missed);
    }
}

static void log_channel_summary(uint8_t ch, const scale_reader_diagnostics_t *d,
                                uint32_t bytes_per_s, uint32_t now_ms)
{
    const board_scale_link_t *link = board_pinmap_get((board_channel_t)ch);
    bool selected = ((board_channel_t)ch == board_pinmap_active_channel());
    log_chan_t c = {
        .ch = (uint8_t)(ch + 1U),
        .uart = link != NULL ? link->uart_port : -1,
        .selected = selected,
        .bytes = d->bytes_received,
        .bytes_per_s = bytes_per_s,
        .frames = d->frames_received,
        .valid = d->valid_frames,
        .valid_st = d->valid_stable,
        .valid_us = d->valid_unstable,
        .rej_len = d->rej_length,
        .rej_hdr = d->rej_header,
        .rej_stat = d->rej_status,
        .rej_field = d->rej_field,
        .rej_other = d->rej_other,
        .rej_term = d->parser_overflows,
        .uart_faults = uart_fault_total(d) - d->parser_overflows,
        .loop_gap_ms = d->loop_gap_max_ms,
        .has_weight = d->has_last_weight,
        .weight_g = d->last_weight_g,
        .stable = d->last_stable,
        .have_age = d->has_last_weight,
        .age_ms = now_ms - (uint32_t)d->last_weight_ms,
        .link = d->link_online ? "ONLINE" : "OFFLINE",
        .have_seq = selected && weight_source_have_valid_cas(),
        .cas_seq = weight_source_cas_sequence(),
        .rej_format = d->rej_format,
        .rej_pad = d->rej_pad,
        .overload = d->overload_frames,
        .stalls = d->reader_stalls,
        .resyncs = d->resync_events,
        .embedded_lf = d->embedded_lf,
    };
    char line[LOG_CHAN_SUMMARY_MAX];
    if (log_fmt_chan_summary(line, sizeof(line), &c) != 0U) {
        ESP_LOGI(TAG_RS485, "%s", line);
    }
}

/* RAW CAPTURE for the bench: what the selected channel really receives, so the
 * undocumented CI-150A pad byte and lamp octet can be read off a healthy scale
 * and strict_pad configured from them. Rate-limited by the caller. */
static void log_raw_capture(uint8_t ch, const scale_reader_diagnostics_t *d)
{
    const board_scale_link_t *link = board_pinmap_get((board_channel_t)ch);
    log_raw_t r = {
        .ch = (uint8_t)(ch + 1U),
        .uart = link != NULL ? link->uart_port : -1,
        .tail = d->raw_tail,
        .tail_len = d->raw_tail_len,
        .have_pad_lamp = d->have_pad_lamp,
        .pad = d->last_pad,
        .lamp = d->last_lamp,
        .embedded_lf = d->embedded_lf,
    };
    char line[LOG_RAW_LINE_MAX];
    if (log_fmt_raw_capture(line, sizeof(line), &r) != 0U) {
        ESP_LOGI(TAG_RS485, "%s", line);
    }
}

static void log_first_ol(uint8_t ch, const scale_reader_diagnostics_t *d)
{
    const board_scale_link_t *link = board_pinmap_get((board_channel_t)ch);
    char line[LOG_RAW_LINE_MAX];
    if (log_fmt_raw_ol(line, sizeof(line), (uint8_t)(ch + 1U),
                       link != NULL ? link->uart_port : -1, d->ol_raw, d->ol_raw_len) != 0U) {
        ESP_LOGW(TAG_RS485, "%s", line);
    }
}

static void diag_task(void *arg)
{
    (void)arg;
    uint32_t period_ms = (uint32_t)LOG_UTIL_HEALTH_PERIOD_S * 1000U;
    if (period_ms != 0U && period_ms < 5000U) period_ms = 5000U; /* Kconfig range is 5..60 */

    log_ratelimit_t fault_rl, drop_rl;
    log_ratelimit_init(&fault_rl);
    log_ratelimit_init(&drop_rl);
    uint32_t last_fault_total = 0U, last_drops = 0U;
    uint32_t last_health_ms = weight_source_now_ms();
    uint32_t seen_transitions[WEIGHT_CHANNEL_COUNT] = {0};
    uint32_t last_bytes[WEIGHT_CHANNEL_COUNT] = {0};
    log_ratelimit_t stall_rl, lf_rl;
    log_ratelimit_init(&stall_rl);
    log_ratelimit_init(&lf_rl);
    uint32_t last_stalls = 0U, last_lf = 0U;
    uint32_t last_raw_ms = weight_source_now_ms();
    bool ol_logged = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAG_TICK_MS));
        uint32_t now_ms = weight_source_now_ms();

        /* Link transitions of BOTH channels, each with its own counters. */
        for (uint8_t ch = 0U; ch < WEIGHT_CHANNEL_COUNT; ch++) {
            scale_reader_diagnostics_t cd;
            if (scale_reader_get_diagnostics(ch, &cd)) {
                log_link_transitions(ch, &cd, &seen_transitions[ch]);
            }
        }

        /* Everything below (SYS line, fault and drop reports) follows the
         * SELECTED channel only. */
        scale_reader_diagnostics_t d;
        bool have_diag = scale_reader_get_diagnostics(board_pinmap_active_channel(), &d);

        if (have_diag) {
            uint32_t faults = uart_fault_total(&d);
            uint32_t folded;
            if (faults != last_fault_total) {
                last_fault_total = faults;
                if (log_ratelimit_event(&fault_rl, now_ms, ERR_LOG_INTERVAL_MS, &folded)) {
                    ESP_LOGW(TAG_RS485, "link faults fifo=%lu ring=%lu break=%lu parity=%lu "
                                        "framing=%lu parser_ovf=%lu evq=%lu read=%lu",
                             (unsigned long)d.fifo_overflows,
                             (unsigned long)d.driver_buffer_overflows,
                             (unsigned long)d.break_events, (unsigned long)d.parity_errors,
                             (unsigned long)d.framing_errors, (unsigned long)d.parser_overflows,
                             (unsigned long)d.uart_event_queue_saturations,
                             (unsigned long)d.uart_read_errors);
                }
            }
            /* Reader stall (RX ring flushed, resynced) and embedded-LF data. */
            if (d.reader_stalls != last_stalls) {
                last_stalls = d.reader_stalls;
                if (log_ratelimit_event(&stall_rl, now_ms, ERR_LOG_INTERVAL_MS, &folded)) {
                    ESP_LOGW(TAG_RS485, "reader stall: loop gap %lums, RX ring flushed, "
                                        "resync to terminator (stalls=%lu)",
                             (unsigned long)d.last_stall_gap_ms,
                             (unsigned long)d.reader_stalls);
                }
            }
            if (d.embedded_lf != last_lf) {
                last_lf = d.embedded_lf;
                if (log_ratelimit_event(&lf_rl, now_ms, ERR_LOG_INTERVAL_MS, &folded)) {
                    ESP_LOGI(TAG_RS485, "embedded LF consumed as CI-150A body data "
                                        "(total=%lu)", (unsigned long)d.embedded_lf);
                }
            }
            if (!ol_logged && d.ol_captured) {
                ol_logged = true;
                log_first_ol(board_pinmap_active_channel(), &d);
            }
            if ((uint32_t)(now_ms - last_raw_ms) >= (uint32_t)CONFIG_SCALE_RAW_CAPTURE_PERIOD_MS) {
                last_raw_ms = now_ms;
                log_raw_capture(board_pinmap_active_channel(), &d);
            }
            if (d.application_queue_drops != last_drops) {
                last_drops = d.application_queue_drops;
                if (log_ratelimit_event(&drop_rl, now_ms, ERR_LOG_INTERVAL_MS, &folded)) {
                    ESP_LOGW(TAG_QUEUE, "scale event queue full, drops=%lu",
                             (unsigned long)d.application_queue_drops);
                }
            }
        }

        if (period_ms == 0U || (uint32_t)(now_ms - last_health_ms) < period_ms) continue;
        uint32_t elapsed_ms = (uint32_t)(now_ms - last_health_ms);
        last_health_ms = now_ms;

        /* 15 s per-channel summary: UART1 and UART2 reported separately, so a
         * silent channel and a busy one can never be confused. */
        for (uint8_t ch = 0U; ch < WEIGHT_CHANNEL_COUNT; ch++) {
            scale_reader_diagnostics_t cd;
            if (!scale_reader_get_diagnostics(ch, &cd)) continue;
            uint32_t delta = cd.bytes_received - last_bytes[ch];
            last_bytes[ch] = cd.bytes_received;
            uint32_t rate = elapsed_ms != 0U
                                ? (uint32_t)(((uint64_t)delta * 1000ULL) / elapsed_ms) : 0U;
            log_channel_summary(ch, &cd, rate, now_ms);
        }

        mqtt_link_stats_t m;
        mqtt_link_get_stats(&m);

        log_health_t h = {
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
            .heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024U),
            .mqtt_up = m.connected,
            .scale_state = cas_link_state_short(weight_source_cas_link_state()),
            .pub = m.published,
            .queue = m.queue_depth,
        };
        if (have_diag) {
            h.rx = d.frames_received;
            h.valid = d.valid_frames;
            h.bad = d.invalid_frames;
        }
        /* Weight only from a real, fresh, valid sample; otherwise n/a. */
        h.weight_valid = weight_source_last_weight_g(&h.weight_g);
        if (weight_source_have_valid_cas()) {
            h.age_valid = true;
            h.age_ms = now_ms - weight_source_last_valid_cas_ms();
        }

        char line[160];
        if (log_fmt_health(line, sizeof(line), &h) != 0U) {
            ESP_LOGI(TAG, "%s", line);
        }
    }
}

/*
 * Inbound control frame from the relay controller. Runs in the httpd task, so
 * it must not block.
 *
 * Production frames accepted here are DIAGNOSTIC ONLY. The relay controller
 * publishes valve state over the same socket so the operator UI can show the
 * nozzle that is open. There is deliberately NO command that can change the
 * weight source, inject a weight, or enable a simulator: a peer on this socket
 * must never be able to substitute a measurement.
 */
static void on_ws_command(const char *payload, size_t len)
{
    if (payload == NULL || len == 0U) return;

    /* A misbehaving peer could repeat these: first one immediately, then one
     * summary per 30 s. The payload is clipped so a frame cannot flood serial. */
    static log_ratelimit_t s_refuse_rl;
    int shown = (len < 80U) ? (int)len : 80;
    uint32_t folded;
    uint32_t now = weight_source_now_ms();

    /* Cheap type probe on the raw frame: only "relay_state" is informative.
     * Any "cmd" frame is a leftover of the removed simulator channel and is
     * refused loudly. */
    static const char k_relay_type[] = "\"type\":\"relay_state\"";
    static const char k_cmd_type[] = "\"type\":\"cmd\"";

    for (size_t i = 0; i + sizeof(k_cmd_type) - 1 <= len; ++i) {
        if (memcmp(payload + i, k_cmd_type, sizeof(k_cmd_type) - 1) == 0) {
            if (log_ratelimit_event(&s_refuse_rl, now, 30000U, &folded)) {
                ESP_LOGW(TAG_CMD, "control frame REFUSED (weight source is not "
                                  "switchable) x%lu: %.*s", (unsigned long)folded,
                         shown, payload);
            }
            return;
        }
    }

    for (size_t i = 0; i + sizeof(k_relay_type) - 1 <= len; ++i) {
        if (memcmp(payload + i, k_relay_type, sizeof(k_relay_type) - 1) == 0) {
            ESP_LOGD(TAG_CMD, "relay state frame: %.*s", shown, payload);
            return;
        }
    }

    if (log_ratelimit_event(&s_refuse_rl, now, 30000U, &folded)) {
        ESP_LOGW(TAG_CMD, "control frame ignored (unrecognised) x%lu: %.*s",
                 (unsigned long)folded, shown, payload);
    }
}

/*
 * One transport handle per channel, allocated statically so the reader task
 * can hold a pointer to it for its whole life. The physical layer is resolved
 * by board_pins.h: RS-485 behind MAX13487E AutoDirection on the current
 * board, or the legacy RS-232 map when that variant is selected. Either way
 * the ESP32 side is an ordinary UART â€” the transceiver owns bus direction.
 */
static scale_transport_rs232_t s_rs232_transport[WEIGHT_CHANNEL_COUNT];

static void start_scale_channel(uint8_t channel_index, int baud)
{
    if (channel_index >= WEIGHT_CHANNEL_COUNT) return;

    scale_link_config_t link_cfg;
    if (!board_pinmap_fill_link((board_channel_t)channel_index, baud, &link_cfg)) {
        ESP_LOGE(TAG_RS485, "channel %u has no board pin map entry",
                 (unsigned)(channel_index + 1U));
        return;
    }

    scale_transport_rs232_t *link = &s_rs232_transport[channel_index];

    esp_err_t err = scale_transport_rs232_init(link, &link_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_RS485, "channel %u transport init failed: %s",
                 (unsigned)(channel_index + 1U), esp_err_to_name(err));
        return;
    }

    const scale_reader_config_t reader_cfg = {
        .channel_id = channel_index,
        .transport = &link->base,
        .protocols = k_protocols,
        .num_protocols = sizeof(k_protocols) / sizeof(k_protocols[0]),
        .event_queue = s_scale_events,
        .allow_auto_detect = SCALE_AUTODETECT_ENABLED,
        .allow_generic_fallback = SCALE_GENERIC_ENABLED,
        .strict_pad = SCALE_STRICT_PAD,
        .pad_value = SCALE_PAD_VALUE,
        .stall_gap_ms = 0U, /* CONFIG_SCALE_READER_STALL_GAP_MS */
    };

    err = scale_reader_start(&reader_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_RS485, "channel %u reader start failed: %s",
                 (unsigned)(channel_index + 1U), esp_err_to_name(err));
        return;
    }
    /* Pin map was already printed once by board_pinmap_log_all(). */
}

void app_main(void)
{
    /* Startup order on the serial console: firmware -> board/pin map ->
     * RS-485 transport -> Wi-Fi -> MQTT -> subscriptions -> scale state ->
     * READY. Tag levels first so nothing below is mis-filtered. */
    log_util_apply_levels();

    /* Build stamp: a stale flash is the most common cause of "code looks
     * right, device behaves old" â€” this line makes a mismatch instant. */
    ESP_LOGI(TAG, "fw=%s build=" __DATE__ " " __TIME__, FIRMWARE_VERSION);

    /* Pin map, UART ownership and physical-layer identity come from
     * board_pins.h â€” one authoritative map, printed before any link opens. */
    board_pinmap_log_all();
    board_pinmap_check_strap_pad();

    /* Golden-vector check on the CI-150A/CI-2001 parser before trusting its output. */
    if (!cas_ci2001_parser_self_test()) {
        ESP_LOGE(TAG, "CAS CI-150A parser self-test failed");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* NVS + device ID â€” same provisioning store as the UART weighing-scale
     * gateway. Wi-Fi credentials are written by the captive portal, not by
     * menuconfig. */
    ESP_ERROR_CHECK(nvs_config_init());
    ESP_ERROR_CHECK(nvs_config_load_defaults());
    ESP_ERROR_CHECK(nvs_config_ensure_device_id());

    /* Explicit scale channel: NVS "scale_ch" (1/2) wins, 0/absent = Kconfig.
     * Never changed at run time and never auto-switched. */
    {
        board_channel_t sel = board_pinmap_kconfig_channel();
        const char *src = "kconfig";
        uint32_t nvs_ch = 0U;
        if (nvs_config_get_u32(NVS_KEY_SCALE_CH, &nvs_ch) == ESP_OK) {
            if (nvs_ch == 1U || nvs_ch == 2U) {
                sel = (board_channel_t)(nvs_ch - 1U);
                src = "nvs";
            } else if (nvs_ch != 0U) {
                ESP_LOGW(TAG, "NVS scale_ch=%lu invalid, using Kconfig",
                         (unsigned long)nvs_ch);
            }
        }
        (void)board_pinmap_set_active_channel(sel);
        weight_source_select_channel((uint8_t)(sel + 1U));
        const board_scale_link_t *l = board_pinmap_get(sel);
        ESP_LOGI(TAG, "scale channel selected: CH%u UART%d TX=GPIO%d RX=GPIO%d "
                      "source=%s (other channel is counted but ignored)",
                 (unsigned)(sel + 1U), l != NULL ? l->uart_port : -1,
                 l != NULL ? l->tx_gpio : -1, l != NULL ? l->rx_gpio : -1, src);
    }

    /* Status LED on GPIO2 (this board has no relay outputs): slow blink =
     * connecting, solid = connected, fast blink = provisioning. Boards that
     * disable the LED (CONFIG_LED_STATUS_GPIO=-1) get log-only hints. */
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

        /* BITS-Scale-XXXX, XXXX from the unique device id (see
         * provisioning_build_ap_name). */
        char dev_id[32] = {0};
        (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, dev_id, sizeof(dev_id));

        char ap_name[64];
        provisioning_build_ap_name("BITS-Scale", dev_id, ap_name, sizeof(ap_name));

        ESP_LOGI(TAG, "Entering forced provisioning mode...");
        provisioning_run(ap_name, PROV_AP_TIMEOUT_MS); /* restarts, does not return */
    }

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    s_scale_events = xQueueCreate(SCALE_EVENT_QUEUE_LEN, sizeof(scale_event_t));
    if (s_scale_events == NULL) {
        ESP_LOGE(TAG, "event queue allocation failed");
        return;
    }

    ESP_ERROR_CHECK(scale_manager_start(s_scale_events));

    start_scale_channel(0U, CONFIG_WEIGHT_DEMO_CH1_BAUD);
    start_scale_channel(1U, CONFIG_WEIGHT_DEMO_CH2_BAUD);

    ESP_ERROR_CHECK(weight_source_start()); /* logs the initial SCALE state */

    ESP_ERROR_CHECK(wifi_manager_start());
    if (wifi_manager_is_started()) {
        if (!wifi_manager_wait_connected(WIFI_CONNECT_TIMEOUT_MS)) {
            ESP_LOGW(TAG_NET, "Wi-Fi connect timed out; still retrying in the background");
        }

        esp_err_t err = wifi_manager_start_mdns();
        if (err != ESP_OK) {
            ESP_LOGW(TAG_NET, "mDNS start failed: %s", esp_err_to_name(err));
        }
    }
    server_health_start();

    /* Reverse channel: the relay controller is a WebSocket client of this
     * server and a WebSocket is bidirectional, so its valve state arrives over
     * the SAME link that carries the weight. This is diagnostics only â€” it
     * drives the dynamic virtual vessel display and nothing in the production
     * weight path. No frame on this socket can change the weight source. */
    websocket_server_set_rx_handler(on_ws_command);

    ESP_ERROR_CHECK(websocket_server_start());

    xTaskCreate(transmit_task, "weight_tx", 5120, NULL, 3, NULL);
    xTaskCreate(diag_task, "diag", 5120, NULL, 1, NULL);

    ESP_LOGI(TAG, "READY");
}
