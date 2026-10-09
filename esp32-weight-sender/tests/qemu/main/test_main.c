/* Logic tests for the weight sender, run as real firmware under QEMU.
 *
 * Only pure components are required, so this suite needs no Wi-Fi. The live
 * scale path, mDNS and the WebSocket server are hardware-verified.
 *
 * Production contract under test here: the weight source has NO simulator and
 * NO fallback. weight_source_get() returns WEIGHT_SOURCE_RESULT_REAL only for
 * a genuinely parsed CAS RS232 sample and WEIGHT_SOURCE_RESULT_NONE otherwise.
 * The pure simulator helpers are compiled ONLY because this QEMU project
 * defines CONFIG_WEIGHT_DEMO_TEST_SIMULATION; they are not a runtime source.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "provisioning.h"
#include "scale_types.h"
#include "test_board_pinmap.h"
#include "test_broker_cfg.h"
#include "test_harness.h"
#include "test_log_util.h"
#include "test_mqtt_cmd.h"
#include "test_mqtt_stop.h"
#include "test_mqtt_env.h"
#include "test_mqtt_stats.h"
#include "test_task_hb_policy.h"
#include "test_transport_golden.h"
#include "test_weight_mqtt_fix.h"
#include "weight_source.h"
#include "websocket_server.h"

static const char *TAG = "TEST";

static int s_pass;
static int s_fail;

static void check(bool condition, const char *name)
{
    if (condition) {
        s_pass++;
        ESP_LOGI(TAG, "TEST_PASS %s", name);
    } else {
        s_fail++;
        ESP_LOGI(TAG, "TEST_FAIL %s", name);
    }
}

/* Shared sink so additional test translation units land in this summary. */
void test_check(bool condition, const char *name)
{
    check(condition, name);
}

void app_main(void)
{
    ESP_LOGI(TAG, "TEST_SUITE_BEGIN");

    check(1 + 1 == 2, "harness_sanity");

    /* ---- Normalisation to grams ---- */
    int grams = 0;
    check(weight_normalize_g(10.0, "kg", &grams) == true && grams == 10000,
          "normalize_10kg_is_10000g");
    check(weight_normalize_g(9.982, "kg", &grams) == true && grams == 9982,
          "normalize_9_982kg_is_9982g");
    check(weight_normalize_g(0.0, "kg", &grams) == true && grams == 0,
          "normalize_zero_is_zero");
    /* lb must convert, so a 10 kg rule still means 10 kg. */
    check(weight_normalize_g(10.0, "lb", &grams) == true && grams == 4536,
          "normalize_10lb_is_4536g");
    check(weight_normalize_g(1.0, "LB", &grams) == true && grams == 454,
          "normalize_unit_case_insensitive");
    check(weight_normalize_g(1.0, "zz", &grams) == false, "normalize_unknown_unit_rejected");
    check(weight_normalize_g(-5.0, "kg", &grams) == true && grams == -5000,
          "normalize_negative_kept_for_receiver_to_reject");

    /* ---- Rounding, deliberately unclamped ---- */
    check(weight_grams_to_kg(10000) == 10, "grams_10000_is_10kg");
    check(weight_grams_to_kg(9982) == 10, "grams_9982_rounds_to_10kg");
    check(weight_grams_to_kg(9400) == 9, "grams_9400_rounds_to_9kg");
    check(weight_grams_to_kg(0) == 0, "grams_0_is_0kg_not_clamped");
    check(weight_grams_to_kg(11000) == 11, "grams_11000_is_11kg_not_clamped");
    check(weight_grams_to_kg(-500) == -1, "grams_negative_not_clamped");

    /* ---- Arbitration: the most recent valid reading wins ---- */
    weight_channel_snapshot_t chans[2];
    weight_sample_t sample;

    chans[0] = (weight_channel_snapshot_t){.valid = true, .stamp_ms = 5000U,
                                           .channel = 1U, .grams = 3000, .stable = true};
    chans[1] = (weight_channel_snapshot_t){.valid = true, .stamp_ms = 7000U,
                                           .channel = 2U, .grams = 10000, .stable = true};
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == true,
          "arbitration_picks_a_valid_channel");
    check(sample.channel == 2U && sample.weight_kg == 10,
          "arbitration_picks_most_recent_channel_2");
    check(sample.source != NULL && strcmp(sample.source, "CAS_RS232") == 0,
          "arbitration_labels_source_cas_rs232");

    chans[0].stamp_ms = 7400U;
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == true,
          "arbitration_still_valid");
    check(sample.channel == 1U, "arbitration_follows_recency_not_channel_number");

    /* Equal timestamps resolve to the lower channel number, deterministically. */
    chans[0].stamp_ms = 7000U;
    chans[1].stamp_ms = 7000U;
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == true,
          "arbitration_tie_is_valid");
    check(sample.channel == 1U, "arbitration_tie_prefers_lower_channel");

    /* A reading older than the window is not usable. */
    chans[0].stamp_ms = 1000U;
    chans[1].stamp_ms = 2000U;
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == false,
          "arbitration_stale_readings_unusable");

    /* One fresh channel is enough even when the other is stale. */
    chans[1].stamp_ms = 7000U;
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == true,
          "arbitration_one_fresh_channel_suffices");
    check(sample.channel == 2U, "arbitration_ignores_stale_channel");

    /* Neither channel valid means no real data at all: publish NOTHING. */
    chans[0].valid = false;
    chans[1].valid = false;
    check(weight_pick_most_recent(chans, 2U, 7500U, 3000U, &sample) == false,
          "arbitration_no_valid_channel_publishes_nothing");

    /* ---- JSON builder: REAL CAS frames only ----
     * Production frames always carry source=CAS_RS232 plus provenance fields.
     * There is no simulated frame shape: a non-scale sample is refused. */
    char json[320];
    const weight_sample_t real = {.weight_kg = 10, .weight_g = 9982,
                                  .from_scale = true, .channel = 1U, .stable = false,
                                  .source = "CAS_RS232", .sequence = 7U,
                                  .stamp_ms = 1000U, .age_ms = 42U,
                                  .channel1_valid = true, .channel1_weight_g = 9982,
                                  .channel1_stable = false, .channel2_valid = true,
                                  .channel2_weight_g = 12014, .channel2_stable = true};
    size_t n = weight_build_json(json, sizeof(json), &real, 25U);
    check(n > 0U, "json_build_returns_length");
    check(strstr(json, "\"source\":\"CAS_RS232\"") != NULL, "json_source_is_cas_rs232");
    check(strstr(json, "\"cas_seq\":7") != NULL, "json_carries_cas_seq");
    check(strstr(json, "\"age_ms\":42") != NULL, "json_carries_age_ms");
    check(strstr(json, "\"sequence\":25") != NULL, "json_carries_tx_sequence");
    check(strstr(json, "\"weight_g\":9982") != NULL, "json_carries_exact_grams");
    check(strstr(json, "\"simulated\"") == NULL, "json_never_claims_simulated");
    check(strstr(json, "\"source\":\"SIM\"") == NULL, "json_never_labels_sim");

    /* Non-scale samples must be refused outright. */
    const weight_sample_t not_scale = {.weight_kg = 10, .weight_g = 9500,
                                       .from_scale = false};
    check(weight_build_json(json, sizeof(json), &not_scale, 26U) == 0U,
          "json_build_refuses_non_scale_sample");

    /* A buffer too small must be refused, never overflowed. */
    char tiny[8];
    check(weight_build_json(tiny, sizeof(tiny), &real, 26U) == 0U,
          "json_build_tiny_buffer_refused");

    /* ---- websocket_server: the no-client path ----
     * With no client attached the generator must not block or fail: the
     * broadcast is a no-op that reports false. The server is not started here
     * (it needs a network stack), so this exercises the guard paths that keep
     * the transmit task safe when nobody is listening. */
    check(websocket_server_has_client() == false, "server_no_client_before_start");
    check(websocket_server_broadcast("{\"type\":\"weight\"}") == false,
          "server_broadcast_without_client_is_false");
    check(websocket_server_broadcast(NULL) == false, "server_broadcast_null_is_false");

    /* ---- Freshness must use the same clock as scale_manager ----
     * scale_manager stamps last_update_ms from esp_timer_get_time()/1000, and
     * esp_timer is initialised before the scheduler starts, so its epoch is
     * offset from the FreeRTOS tick epoch. Computing "now" from the tick
     * counter makes every real reading look stale, which would silence a
     * perfectly good CAS scale. */
    uint32_t now_et = weight_source_now_ms();
    uint32_t et = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t skew = (now_et > et) ? (now_et - et) : (et - now_et);
    check(skew < 1000U, "now_ms_matches_esp_timer_epoch");

    weight_channel_snapshot_t live[1];
    live[0] = (weight_channel_snapshot_t){
        .valid = true,
        .stamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .channel = 1U,
        .grams = 10000,
        .stable = true,
    };
    check(weight_pick_most_recent(live, 1U, weight_source_now_ms(), 3000U, &sample) == true,
          "fresh_esp_timer_stamped_reading_is_accepted");
    check(sample.age_ms < 1000U, "fresh_reading_age_is_small");

    /* ---- Value selection from a scale reading ----
     * Prefer the display value, fall back to gross when the parser reports
     * only gross (asuki_parser sets gross and never display), and reject a
     * value the parser itself flagged as non-physical. */
    scale_reading_t rd;

    memset(&rd, 0, sizeof(rd));
    rd.valid = true;
    rd.has_display = true;
    rd.display = 10.0;
    rd.display_is_physical = true;
    rd.has_unit = true;
    strcpy(rd.unit, "kg");
    bool used_display = false;
    check(weight_sample_from_reading(&rd, 1U, &sample, &used_display) == true, "reading_display_used");
    check(used_display, "reading_display_flag_set");
    check(sample.weight_kg == 10 && sample.weight_g == 10000, "reading_display_value");
    check(sample.source != NULL && strcmp(sample.source, "CAS_RS232") == 0,
          "reading_labels_source_cas_rs232");

    memset(&rd, 0, sizeof(rd));
    rd.valid = true;
    rd.has_gross = true;
    rd.gross = 3.0;
    rd.gross_is_physical = true;
    rd.has_unit = true;
    strcpy(rd.unit, "kg");
    used_display = true;
    check(weight_sample_from_reading(&rd, 2U, &sample, &used_display) == true, "reading_gross_fallback_used");
    check(!used_display, "reading_gross_flag_clear");
    check(sample.weight_kg == 3 && sample.channel == 2U, "reading_gross_value");

    memset(&rd, 0, sizeof(rd));
    rd.valid = true;
    rd.has_display = true;
    rd.display = 10.0;
    rd.display_is_physical = false;
    rd.has_unit = true;
    strcpy(rd.unit, "kg");
    check(weight_sample_from_reading(&rd, 1U, &sample, NULL) == false, "reading_non_physical_rejected");

    memset(&rd, 0, sizeof(rd));
    rd.valid = true;
    rd.has_display = true;
    rd.display = 10.0;
    rd.display_is_physical = true;
    check(weight_sample_from_reading(&rd, 1U, &sample, NULL) == false, "reading_missing_unit_rejected");

    memset(&rd, 0, sizeof(rd));
    check(weight_sample_from_reading(&rd, 1U, &sample, NULL) == false, "reading_invalid_rejected");

    /* ---- The raw-frame log toggle must actually be applied ---- */
    weight_source_init_logging();
    check(esp_log_level_get("RS485") == ESP_LOG_INFO, "frame_logging_off_by_default");

    /* ---- Sequence: peek never burns a number, advance moves it by one ----
     * The counter must move ONLY for messages that actually went out, so a
     * failed/skipped broadcast cannot make a reconnecting peer see a gap. */
    weight_source_start(); /* resets the counter to 1 */
    check(weight_source_peek_sequence() == 1U, "sequence_starts_at_1");
    check(weight_source_peek_sequence() == 1U, "peek_is_idempotent");
    weight_source_advance_sequence();
    check(weight_source_peek_sequence() == 2U, "advance_moves_by_one");
    weight_source_advance_sequence();
    check(weight_source_peek_sequence() == 3U, "advance_again_moves_by_one");

    /* ---- CAS provenance state machine ----
     * ONLINE is reached ONLY after a valid frame has been parsed. Starting the
     * task must never claim ONLINE. */
    weight_source_start();
    check(weight_source_have_valid_cas() == false, "cas_start_has_no_valid_frame");
    check(weight_source_cas_link_state() == CAS_LINK_WAITING,
          "cas_link_starts_waiting");
    check(weight_source_last_valid_cas_ms() == 0U, "cas_last_valid_starts_zero");
    check(weight_source_cas_sequence() == 0U, "cas_sequence_starts_zero");

    weight_source_log_cas_status(weight_source_now_ms());
    check(weight_source_cas_link_state() == CAS_LINK_WAITING,
          "cas_link_stays_waiting_without_frame");

    check(strcmp(cas_link_state_name(CAS_LINK_WAITING), "WAITING FOR DATA") == 0,
          "cas_state_name_waiting");
    check(strstr(cas_link_state_name(CAS_LINK_ONLINE), "ONLINE") != NULL,
          "cas_state_name_online");
    check(strcmp(cas_link_state_name(CAS_LINK_STALE), "STALE") == 0,
          "cas_state_name_stale");
    check(strcmp(cas_link_state_name(CAS_LINK_OFFLINE), "OFFLINE") == 0,
          "cas_state_name_offline");

    /* ---- Production weight_source_get with no scale manager ----
     * scale_manager is deliberately NOT started in this suite, which is
     * exactly the "no real scale attached" condition. Production must
     * publish NOTHING. There is no simulator, no AUTO fallback, no
     * last-known-as-fresh and no zero. */
    weight_source_result_t src = weight_source_get(&sample);
    check(src == WEIGHT_SOURCE_RESULT_NONE,
          "no_scale_publishes_nothing");
    check(weight_source_have_valid_cas() == false,
          "no_scale_does_not_claim_valid_cas");
    check(weight_source_cas_link_state() == CAS_LINK_WAITING,
          "no_scale_link_stays_waiting");

    /* A second call must also produce nothing — no cached-as-fresh leak. */
    check(weight_source_get(&sample) == WEIGHT_SOURCE_RESULT_NONE,
          "no_scale_second_call_also_nothing");

    /* ---- Provisioning AP name: BITS-Scale-XXXX from the unique device id ----
     * XXXX is the last four hex characters of device_id, upper-cased, and is
     * scanned from the END so the derivation survives a change to the id's
     * textual prefix. A short or non-hex id must still yield a usable name —
     * an unnamed SoftAP is indistinguishable from another device's. */
    char ap[64];

    provisioning_build_ap_name("BITS-Scale", "bits-a4cf12ab34cd", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-34CD") == 0, "ap_name_scale_from_device_id");

    provisioning_build_ap_name("BITS-Relay", "bits-a4cf12ab34cd", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Relay-34CD") == 0, "ap_name_relay_from_device_id");

    provisioning_build_ap_name("BITS-Scale", "bits-0000000000ab", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-00AB") == 0, "ap_name_short_hex_run_padded");

    provisioning_build_ap_name("BITS-Scale", "bits-ab", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-00AB") == 0, "ap_name_two_hex_padded");

    provisioning_build_ap_name("BITS-Scale", NULL, ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-0000") == 0, "ap_name_null_id_is_zero_suffix");

    provisioning_build_ap_name("BITS-Scale", "", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-0000") == 0, "ap_name_empty_id_is_zero_suffix");

    provisioning_build_ap_name("BITS-Scale", "devices", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-0000") == 0, "ap_name_no_trailing_hex_is_zero_suffix");

    /* Lower case is normalised up: an AP name that differs only in case is the
     * same network to a user reading it off a phone screen. */
    provisioning_build_ap_name("BITS-Scale", "bits-112233aabbcc", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-BBCC") == 0, "ap_name_upper_cases_suffix");

    /* A tight buffer must truncate, never overflow, and stay NUL-terminated. */
    char small[12];
    memset(small, 'X', sizeof(small));
    provisioning_build_ap_name("BITS-Scale", "bits-a4cf12ab34cd", small, sizeof(small));
    check(small[sizeof(small) - 1U] == '\0', "ap_name_small_buffer_terminated");
    check(strlen(small) == sizeof(small) - 1U, "ap_name_small_buffer_truncated");

    char zero_cap[4] = {'a', 'b', 'c', 'd'};
    provisioning_build_ap_name("BITS-Scale", "bits-a4cf12ab34cd", zero_cap, 0U);
    check(zero_cap[0] == 'a', "ap_name_zero_capacity_writes_nothing");

    /* ---- Inbound RX handler registration is safe when no server exists ---- */
    websocket_server_set_rx_handler(NULL);
    check(true, "rx_handler_registration_is_safe");

#ifdef CONFIG_WEIGHT_DEMO_TEST_SIMULATION
    /* ---- TEST-ONLY pure simulator helpers ----
     * These are compiled solely because this QEMU project defines
     * CONFIG_WEIGHT_DEMO_TEST_SIMULATION. They are not a runtime weight source
     * and production firmware does not have them. */

    /* Deterministic progression, used only by host tests. */
    check(weight_sim_step_g(0U) == 8000, "sim_step_0_is_8kg");
    check(weight_sim_step_g(3U) == 10000, "sim_step_3_is_10kg_at_trigger");
    check(weight_sim_step_g(4U) == 12000, "sim_step_4_is_12kg_above_trigger");
    check(weight_sim_step_g(8U) == 8000, "sim_progression_wraps");

    /* Virtual vessel: the pure flow integrator.
     * Integer grams; the clamp is a safety property, not a nicety — an
     * overflowing vessel would be published as a real weight. */
    check(weight_sim_integrate_g(0, false, false, 1000U) == 0, "integrate_no_valve_no_flow");
    check(weight_sim_integrate_g(0, true, false, 1000U) == WEIGHT_SIM_COARSE_RATE_G_S,
          "integrate_coarse_one_second");
    check(weight_sim_integrate_g(0, false, true, 1000U) == WEIGHT_SIM_FINE_RATE_G_S,
          "integrate_fine_one_second");
    check(weight_sim_integrate_g(0, true, true, 1000U) ==
              WEIGHT_SIM_COARSE_RATE_G_S + WEIGHT_SIM_FINE_RATE_G_S,
          "integrate_both_valves_add");
    check(weight_sim_integrate_g(1000, true, false, 500U) ==
              1000 + WEIGHT_SIM_COARSE_RATE_G_S / 2,
          "integrate_half_second_is_proportional");
    check(weight_sim_integrate_g(1000, true, false, 0U) == 1000, "integrate_zero_dt_no_change");
    check(weight_sim_integrate_g(-50, false, false, 1000U) == 0, "integrate_negative_floors_at_zero");
    check(weight_sim_integrate_g(WEIGHT_SIM_MAX_G - 1, true, true, 60000U) == WEIGHT_SIM_MAX_G,
          "integrate_clamps_at_ceiling");
    check(weight_sim_integrate_g(0, true, true, 0xFFFFFFFFU) == WEIGHT_SIM_MAX_G,
          "integrate_huge_dt_clamps_not_wraps");
    check(weight_sim_integrate_g(0, false, true, 1000U) <
          weight_sim_integrate_g(0, true, false, 1000U),
          "fine_valve_fills_slower_than_coarse");

    /* Deterministic ramp: a pure function of elapsed time. */
    check(WEIGHT_RAMP_STEP_G == 1000, "ramp_step_is_1000g");
    check(WEIGHT_RAMP_TOP_G == 20000, "ramp_top_is_20000g");
    check(WEIGHT_RAMP_STEPS == 20, "ramp_has_20_steps");
    check(WEIGHT_RAMP_TOP_MS == 20000, "ramp_reaches_top_at_20s");
    check(WEIGHT_RAMP_HOLD_MS == 5000, "ramp_holds_5s");
    check(WEIGHT_RAMP_CYCLE_MS == 25000, "ramp_cycle_is_25s");

    weight_ramp_sample_t r;

    weight_ramp_at(0U, &r);
    check(r.weight_g == 0 && r.cycle == 0U && !r.holding, "ramp_starts_at_zero");

    bool steps_exact = true;
    for (uint32_t i = 0U; i <= WEIGHT_RAMP_STEPS; i++) {
        weight_ramp_at(i * WEIGHT_RAMP_STEP_MS, &r);
        if (r.weight_g != (int32_t)(i * WEIGHT_RAMP_STEP_G)) steps_exact = false;
        if (r.cycle != 0U) steps_exact = false;
    }
    check(steps_exact, "ramp_publishes_every_1000g_step");

    weight_ramp_at(500U, &r);
    check(r.weight_g == 0, "ramp_holds_value_between_steps");
    weight_ramp_at(1000U, &r);
    check(r.weight_g == 1000, "ramp_first_increment_at_one_second");
    weight_ramp_at(5000U, &r);
    check(r.weight_g == 5000, "ramp_is_5kg_at_five_seconds");

    weight_ramp_at(20000U, &r);
    check(r.weight_g == 20000 && r.holding, "ramp_top_is_held_not_overshot");
    weight_ramp_at(21000U, &r);
    check(r.weight_g == 20000 && r.holding, "ramp_holds_at_21s");
    weight_ramp_at(24999U, &r);
    check(r.weight_g == 20000 && r.holding, "ramp_holds_until_the_very_end");
    check(r.cycle == 0U, "ramp_hold_stays_in_the_same_cycle");

    weight_ramp_at(25000U, &r);
    check(r.weight_g == 0, "ramp_resets_to_zero_after_hold");
    check(r.cycle == 1U, "ramp_reset_increments_cycle");
    check(!r.holding, "ramp_reset_leaves_the_hold");

    check(weight_ramp_is_reset(20000, 0), "ramp_reset_detected_top_to_zero");
    check(!weight_ramp_is_reset(0, 1000), "ramp_step_up_is_not_a_reset");
    check(!weight_ramp_is_reset(5000, 0), "ramp_mid_ramp_zero_is_not_a_reset");

    /* These helpers must never become a production weight source. Prove the
     * production get() still returns NONE even when the helpers exist. */
    check(weight_source_get(&sample) == WEIGHT_SOURCE_RESULT_NONE,
          "test_helpers_never_become_a_weight_source");
#else
    check(true, "test_sim_helpers_not_compiled_into_production");
#endif

    /* ---- RS232 golden reference (pre-transport-abstraction baseline) ----
     * See docs/superpowers/baseline/rs232-golden/GOLDEN_BASELINE.md. These
     * pin parser output, the framing layer and the transport event contract
     * so a transport refactor cannot silently change behaviour. */
    test_mqtt_stats_run();
    test_task_hb_policy_run();

    /* ---- MQTT link core + remote ZERO/TARE command framework ----
     * Topics, LWT/birth, backoff, non-blocking bounded queue, command validation,
     * dedupe, busy/offline/retained refusal and the VERIFICATION REQUIRED path. */
    test_mqtt_cmd_run();
    test_mqtt_stop_run();
    test_mqtt_env_run();
    test_weight_mqtt_fix_run();
    test_broker_cfg_run();

    /* ---- Serial-log policy helpers (change detector, rate limiter, health line) ---- */
    test_log_util_run();

    test_transport_golden_run();

    /* ---- Board pin map (RS-485 / UART / strap-pad contract) ----
     * Locks the single authoritative GPIO/UART mapping, console-UART
     * protection, MAX13487E AutoDirection (no DE/RE pad), and the GPIO15
     * strap-pad / single-scale channel selection. */
    test_board_pinmap_run();

    extern void test_responsiveness_run(void);
    test_responsiveness_run();

    ESP_LOGI(TAG, "TEST_SUITE_END pass=%d fail=%d", s_pass, s_fail);
    vTaskDelay(pdMS_TO_TICKS(200));
}
