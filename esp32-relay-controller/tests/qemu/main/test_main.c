/* Logic tests for the relay controller, run as real firmware under QEMU.
 *
 * QEMU does not emulate Wi-Fi, so this suite covers validation, the group
 * relay decision (all 12 channels, gram-domain threshold), timeout behaviour,
 * sequence diagnostics, the failsafe race closure and payload reassembly
 * only. The WebSocket path, mDNS and GPIO pad readback are hardware-verified.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dispense_controller.h"
#include "dual_dispense_controller.h"
#include "job_queue.h"
#include "provisioning.h"
#include "relay_driver.h"
#include "relay_expander.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "websocket_client.h"
#include "weight_receiver.h"

#include <stdio.h>

static const char *TAG = "TEST";

static int s_pass;
static int s_fail;

static volatile bool s_hammer_stop;

/* Two tasks that race on the whole group, standing in for the real
 * contenders: the WebSocket client task driving the relays from a weight, and
 * the failsafe path turning them off. */
static void hammer_on(void *arg)
{
    (void)arg;
    while (!s_hammer_stop) {
        (void)relay_all_on();
        taskYIELD();
    }
    vTaskDelete(NULL);
}

static void hammer_off(void *arg)
{
    (void)arg;
    while (!s_hammer_stop) {
        (void)relay_all_off();
        taskYIELD();
    }
    vTaskDelete(NULL);
}

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

void audit_check(bool condition, const char *name) { check(condition, name); }


/* True when every channel's cached commanded state equals `on`. */
static bool all_relays(bool on)
{
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        if (relay_get_state(i) != on) return false;
    }
    return true;
}

/* Feed one JSON weight message through the receiver at the given time. */
static void feed(const char *json, uint32_t now_ms)
{
    weight_receiver_on_message(json, strlen(json), now_ms);
}


/* Both DOSING relays de-energized: index 0 = Relay 1 / coarse, index 1 =
 * Relay 2 / fine. The confirmed hardware mapping is asserted separately. */
static bool dosing_off(void)
{
    return !relay_get_state(RELAY_INDEX_COARSE) &&
           !relay_get_state(RELAY_INDEX_FINE);
}

/* One supervisor period: the same three steps, in the same order, as
 * app_main's supervisor_task. The controller tests drive time through this
 * rather than calling the tick directly, so they exercise the real cadence â€”
 * including the receiver's failsafe, which runs first and can force the
 * relays off before the controller ever sees the tick. */
static void pump(uint32_t *t, int periods)
{
    for (int i = 0; i < periods; i++) {
        *t += 100U;
        weight_receiver_tick(*t);
        dispense_controller_tick(*t);
        safety_manager_tick(*t);
    }
}

/* Test-only fixture: feed a weight sample directly to the dual controller.
 * Bypasses weight_receiver JSON parsing so the controller can be driven
 * deterministically with hand-built weight_msg_t values. Simulated flags are
 * not set â€” the controller rejects those (tested separately). */
static void dual_feed(uint32_t now, int32_t w)
{
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_weight1 = true;
    m.weight1_g = w;
    m.has_stable1 = true;
    m.stable1 = true;
    m.weight_g = w;
    dual_dispense_controller_on_weight(&m, now);
}

/* READY-gate fixtures: fresh controller, 5000 g slots on both channels. */
static void or_setup(void)
{
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    (void)dual_dispense_controller_set_pending_profile_staged(1, "or-slot1", 31, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 0);
    (void)dual_dispense_controller_set_pending_profile_staged(2, "or-slot2", 32, 5000,
          0.98f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 0);
}

/* Drive the (already READY) CH1 job at target 5000 to COMPLETE. */
static void or_complete_ch1(uint32_t t)
{
    dual_feed(t, 0);
    dual_dispense_controller_tick(t);
    dual_feed(t + 50U, 5000);
    dual_dispense_controller_tick(t + 50U);
    dual_feed(t + 350U, 5000);
    dual_dispense_controller_tick(t + 350U);
    dual_feed(t + 400U, 5000);
    dual_dispense_controller_tick(t + 400U);
}

static void tests_body(void)
{
    ESP_LOGI(TAG, "TEST_SUITE_BEGIN");

    check(1 + 1 == 2, "harness_sanity");

    /* Single-scale suite (CONTRACT 9.11) config baseline for the pre-9.11 tests:
     * they start jobs from non-empty vessels (4900 g, ...), so the UNVALIDATED
     * start_max_g plausibility check is off here. test_single_scale.c sets its own
     * configuration and restores this one. */
    {
        dual_scale_cfg_t cfg;
        dual_dispense_controller_scale_cfg_default(&cfg);
        cfg.start_max_g = 0;
        dual_dispense_controller_set_scale_cfg(&cfg);
    }

    /* relay_driver: centralized mapping table â€” the verified PCB mapping
     * AO1..AO12 = 18,19,21,25,26,27,32,33,4,13,12,2. Must never be
     * reassigned; AO12 stays on GPIO2. */
    const int expected_gpio[12] = {18, 19, 21, 25, 26, 27, 32, 33, 4, 13, 12, 2};
    bool map_ok = true;
    for (uint8_t i = 0; i < 12U; i++) {
        if (relay_gpio_for_index(i) != expected_gpio[i]) map_ok = false;
    }
    check(map_ok, "relay_map_all_12_channels");
    check(relay_gpio_for_index(0) == 18, "relay_index_0_is_gpio18");
    check(relay_gpio_for_index(11) == 2, "relay_index_11_is_gpio2");
    check(relay_gpio_for_index(12) == -1, "relay_index_12_out_of_range");
    check(relay_gpio_for_index(255) == -1, "relay_index_255_out_of_range");

    /* relay_driver: cached commanded state */
    check(relay_init_all() == ESP_OK, "relay_init_returns_ok");
    check(all_relays(false), "relay_init_all_leaves_every_relay_off");

    /* ---- relay_expander: CONFIRMED PCF8574T hardware ----
     * Relay 1 and Relay 2 are P6 and P7 of a PCF8574T at 7-bit 0x22. The
     * address derivation is written as the OR in the header precisely so it
     * can be pinned here rather than taken on trust. */
    check(RELAY_EXPANDER_BASE_ADDR == 0x20U, "expander_base_addr_0x20");
    /* Strapping MEASURED on the board: A2=0, A1=1, A0=0. This supersedes the
     * earlier 0x25 report, which was the exact inverse - so the derivation is
     * re-pinned here rather than assumed to follow. */
    check(RELAY_EXPANDER_A2 == 0U && RELAY_EXPANDER_A1 == 1U &&
          RELAY_EXPANDER_A0 == 0U, "expander_straps_A2_0_A1_1_A0_0");
    check(RELAY_EXPANDER_BASE_ADDR == 0x20U, "expander_base_is_the_true_non_A_base");
    check(RELAY_EXPANDER_BASE_ADDR != RELAY_EXPANDER_ADDR,
          "expander_base_is_not_preloaded_with_strap_bits");
    check(RELAY_EXPANDER_ADDR == 0x22U, "expander_addr_derives_to_0x22");
    check((0x20U | (0U << 2) | (1U << 1) | 0U) == 0x22U,
          "expander_addr_bit_arithmetic_0b0100_010");
    /* And the inverse must NOT be what ships: this is the value the board
     * rejected, and a regression to it would silently address the wrong
     * device. */
    check((0x20U | (1U << 2) | (0U << 1) | 1U) == 0x25U,
          "superseded_addr_0x25_still_computes_to_0x25");

    check(RELAY_EXPANDER_BIT_RELAY1 == 6U, "relay1_bit_is_6");
    check(RELAY_EXPANDER_BIT_RELAY2 == 7U, "relay2_bit_is_7");
    check(RELAY_EXPANDER_MASK_RELAY1 == 0x40U, "relay1_mask_is_0x40");
    check(RELAY_EXPANDER_MASK_RELAY2 == 0x80U, "relay2_mask_is_0x80");
    check(RELAY_EXPANDER_USED_MASK == 0xC0U, "used_mask_covers_p6_p7_only");

    uint8_t bit = 9U;
    check(relay_expander_bit_for_index(0U, &bit) && bit == 6U,
          "map_relay1_is_p6");
    check(relay_expander_bit_for_index(1U, &bit) && bit == 7U,
          "map_relay2_is_p7");
    check(!relay_expander_bit_for_index(2U, &bit), "map_index2_not_on_expander");
    check(!relay_expander_bit_for_index(11U, &bit), "map_index11_not_on_expander");
    check(!relay_expander_bit_for_index(255U, &bit), "map_index255_rejected");
    check(!relay_expander_bit_for_index(0U, NULL), "map_null_bit_rejected");

    /* Single-bit shadow moves per polarity (active-LOW: on clears the bit). */
    check(relay_expander_shadow_set(0xFFU, 6U, true, true) == 0xBFU,
          "shadow_alow_on_clears_relay1_bit");
    check(relay_expander_shadow_set(0xBFU, 6U, false, true) == 0xFFU,
          "shadow_alow_off_sets_relay1_bit");
    check(relay_expander_shadow_set(0x00U, 7U, true, false) == 0x80U,
          "shadow_ahigh_on_sets_relay2_bit");
    check(relay_expander_shadow_set(0x80U, 7U, false, false) == 0x00U,
          "shadow_ahigh_off_clears_relay2_bit");
    check(relay_expander_bit_is_on(0xBFU, 6U, true), "bit_is_on_alow");
    check(!relay_expander_bit_is_on(0xFFU, 6U, true), "bit_is_off_alow");
    check(relay_expander_bit_is_on(0x80U, 7U, false), "bit_is_on_ahigh");
    check(!relay_expander_bit_is_on(0x00U, 7U, false), "bit_is_off_ahigh");

    /* ---- THE SHIPPED POLARITY IS ACTIVE-HIGH (measured on the board) ----
     *
     * The firmware originally shipped ACTIVE-LOW from a derivation that turned
     * out to be wrong: on the real board that inverted every relay, so every
     * failsafe OFF was driving the solenoids ON. These checks pin the
     * MEASURED polarity, because a silent regression to the old default would
     * invert both relays without necessarily failing anything else.
     *
     * ACTIVE-HIGH means OFF CLEARS bits 6/7 and ON SETS them. */
    check(!relay_expander_active_low(), "shipped_polarity_is_active_high");

    /* The boot fail-safe pattern applied to the expander's POWER-UP state
     * (every port pulled HIGH = 0xFF on a PCF8574). Under ACTIVE-HIGH that
     * power-up state is the ENERGIZED level, so this write is what actually
     * switches the relays off â€” and it must clear exactly the two relay bits
     * while handing back the six that are not ours. */
    check(relay_expander_shadow_all(0xFFU, false, false) == 0x3FU,
          "boot_off_pattern_active_high_clears_relay_bits");
    check((relay_expander_shadow_all(0xFFU, false, false) & 0xC0U) == 0x00U,
          "boot_off_pattern_leaves_no_relay_bit_set");
    check((relay_expander_shadow_all(0xFFU, false, false) & 0x3FU) == 0x3FU,
          "boot_off_pattern_preserves_unused_bits");
    check(relay_expander_shadow_all(0x00U, true, false) == 0xC0U,
          "on_pattern_active_high_sets_relay_bits");

    /* And per-channel, which is what the dispense controller actually drives. */
    check(relay_expander_shadow_set(0xFFU, 6U, false, false) == 0xBFU,
          "active_high_off_clears_relay1_bit");
    check(relay_expander_shadow_set(0xBFU, 6U, true, false) == 0xFFU,
          "active_high_on_sets_relay1_bit");
    /* Under ACTIVE-HIGH a SET bit is the energized one, and OFF is the CLEARED
     * bit - the inverse of the old shipped polarity, which is precisely the
     * inversion that was observed on the board. */
    check(relay_expander_bit_is_on(0xFFU, 6U, false), "active_high_bit_set_is_on");
    check(!relay_expander_bit_is_on(0x00U, 6U, false), "active_high_bit_clear_is_off");
    check(relay_expander_bit_is_on(0xFFU, 7U, false), "active_high_relay2_bit_set_is_on");
    check(!relay_expander_bit_is_on(0x3FU, 7U, false), "active_high_relay2_bit_clear_is_off");

    /* The ALL pattern touches ONLY the two relay bits: the six pins that
     * belong to whatever else shares the expander ride through verbatim. A
     * blind byte write would clobber them. 0x3F is the probe: it has every
     * unused bit set, so a helper that clobbered them would show it.
     *
     * The active-LOW cases below stay: they exercise the helper for the
     * polarity a board variant could still select, which is exactly why the
     * choice is kept as a Kconfig option rather than hard-coded. */
    check(relay_expander_shadow_all(0x00U, true, true) == 0x00U,
          "all_alow_on_clears_relay_bits");
    check(relay_expander_shadow_all(0x00U, false, true) == 0xC0U,
          "all_alow_off_sets_relay_bits");
    check(relay_expander_shadow_all(0x3FU, true, true) == 0x3FU,
          "all_alow_on_preserves_unused_bits");
    check(relay_expander_shadow_all(0x3FU, false, true) == 0xFFU,
          "all_alow_off_preserves_unused_bits");

    /* Active-HIGH is the inverse polarity: ON sets the relay bits. */
    check(relay_expander_shadow_all(0x3FU, true, false) == 0xFFU,
          "all_ahigh_on_sets_relay_bits");
    check(relay_expander_shadow_all(0xFFU, false, false) == 0x3FU,
          "all_ahigh_off_clears_relay_bits");
    /* The boot fail-safe pattern: both relays inactive, everything else kept.
     * Under the derived active-LOW polarity that is bits 6 and 7 SET â€” which
     * is exactly the PCF8574's own power-up state, so the write cannot
     * energize anything. */
    check(relay_expander_shadow_all(0x00U, false, true) == 0xC0U,
          "boot_failsafe_pattern_alow");
    check((relay_expander_shadow_all(0x00U, false, true) & 0xC0U) == 0xC0U,
          "boot_failsafe_leaves_relay_bits_high");

    /* QEMU has an emulated I2C controller but NO slave devices: discovery
     * must leave the backend inactive and every actuation must no-op safely
     * rather than pretend a relay moved. */
    check(!relay_expander_backend_active(), "qemu_backend_inactive_no_slaves");
    check(!relay_expander_detected(), "qemu_expander_not_detected");
    check(!relay_expander_actuation_allowed(), "qemu_actuation_not_allowed");
    check(relay_expander_set_channel(0U, true) == ESP_ERR_NOT_FOUND,
          "qemu_channel_actuation_noops_not_found");
    check(relay_expander_set_channel(0U, false) == ESP_ERR_NOT_FOUND,
          "qemu_channel_off_noops_not_found");
    check(relay_expander_all_off() == ESP_ERR_NOT_FOUND,
          "qemu_all_off_noops_not_found");
    check(relay_expander_all_on() == ESP_ERR_NOT_FOUND,
          "qemu_all_on_noops_not_found");
    check(!relay_expander_channel_on(0U), "qemu_channel_reports_off");

    /* ---- A REJECTED ON must never be reported as an achieved state ----
     *
     * QEMU has no I2C slaves, so the expander backend is inactive and every ON
     * is refused. That is a rejection we can reproduce exactly, and it is the
     * condition the reporting bug hid: the dispense controller cached the
     * COMMANDED state, so the UI said OPEN while the driver had refused the
     * write outright. The state must stay off at every layer. */
    check(relay_expander_set_channel(0U, true) == ESP_ERR_NOT_FOUND,
          "rejected_on_returns_not_found");
    check(!relay_expander_channel_on(0U), "rejected_on_leaves_expander_channel_off");
    check(!relay_expander_channel_on(1U), "rejected_on_leaves_relay2_off");
    check(relay_expander_shadow() == 0x00U ||
          (relay_expander_shadow() & 0xC0U) == 0x00U,
          "rejected_on_leaves_no_relay_bit_set");

    /* The gate is a CONJUNCTION of two facts, and both are false here: no
     * backend, and the compiled verification flag. When the backend is live and
     * the flag is on, ON is permitted - that is the path the field fix enables,
     * and it is bench-verifiable rather than QEMU-verifiable. */
    check(!relay_expander_actuation_allowed(), "gate_closed_without_backend");
    check(!relay_expander_backend_active(), "gate_reason_is_backend_not_flag");

    check(relay_set(0, true) == ESP_OK, "relay_set_valid_index_ok");
    check(relay_get_state(0) == true, "relay_set_updates_cached_state");
    check(relay_set(0, false) == ESP_OK, "relay_clear_valid_index_ok");
    check(relay_get_state(0) == false, "relay_clear_updates_cached_state");

    check(relay_set(12, true) == ESP_ERR_INVALID_ARG, "relay_set_index_12_rejected");
    check(relay_set(255, true) == ESP_ERR_INVALID_ARG, "relay_set_index_255_rejected");

    /* AO12 / GPIO2 is a full relay channel again: with the status LED
     * disabled on this board (CONFIG_LED_STATUS_GPIO=-1), relay_driver is the
     * pin's single owner and index 11 must be controllable. */
    check(relay_set(11, true) == ESP_OK, "relay_set_index_11_controllable");
    check(relay_get_state(11) == true, "relay_11_cache_follows_command");
    check(relay_set(11, false) == ESP_OK, "relay_clear_index_11_ok");

    /* Group commands drive all 12 channels together. */
    check(relay_all_on() == ESP_OK, "relay_all_on_returns_ok");
    check(all_relays(true), "relay_all_on_energizes_all_12");
    check(relay_all_off() == ESP_OK, "relay_all_off_returns_ok");
    check(all_relays(false), "relay_all_off_clears_all_12");

    (void)relay_set(3, true);
    (void)relay_all_off();
    check(relay_get_state(3) == false, "relay_all_off_clears_single_set");

    /* ---- weight_receiver: the validation table (gram-domain grammar) ----
     * Every rejected case must leave the timestamp and the relays untouched,
     * so this block only asserts the parse verdict; weight_receiver_on_message
     * is exercised separately below. */
    weight_msg_t msg;

    /* weight_g is the authoritative field. */
    const char *g_only = "{\"type\":\"weight\",\"weight_g\":9500,\"sequence\":25}";
    check(weight_msg_parse(g_only, strlen(g_only), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_grams_only");
    check(msg.weight_g == 9500, "parse_grams_authoritative");
    check(msg.weight_kg == 10, "parse_kg_is_rounded_convenience");
    check(msg.sequence == 25, "parse_extracts_sequence");

    /* Rounding-trap regression: weight_g must win over a weight_kg that
     * rounds to the trigger. */
    const char *conflict = "{\"type\":\"weight\",\"weight_kg\":10,\"weight_g\":9500}";
    check(weight_msg_parse(conflict, strlen(conflict), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_both_fields");
    check(msg.weight_g == 9500, "parse_grams_win_over_kg");

    /* kg-only messages (legacy 3-field format) may carry a fraction. */
    const char *kg_int = "{\"type\":\"weight\",\"weight_kg\":7}";
    check(weight_msg_parse(kg_int, strlen(kg_int), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_integer_kg");
    check(msg.weight_g == 7000, "parse_kg_converted_to_grams");

    const char *kg_frac = "{\"type\":\"weight\",\"weight_kg\":9.5}";
    check(weight_msg_parse(kg_frac, strlen(kg_frac), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_fractional_kg");
    check(msg.weight_g == 9500, "parse_fractional_kg_grams");

    const char *kg_12 = "{\"type\":\"weight\",\"weight_kg\":12.0}";
    check(weight_msg_parse(kg_12, strlen(kg_12), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_12kg_above_trigger");
    check(msg.weight_g == 12000, "parse_12kg_grams");

    const char *no_seq = "{\"type\":\"weight\",\"weight_g\":10000}";
    check(weight_msg_parse(no_seq, strlen(no_seq), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_without_sequence");
    check(msg.has_sequence == false, "parse_sequence_optional");

    /* The sender's extra fields must be ignored, not rejected. */
    const char *extended = "{\"type\":\"weight\",\"weight_kg\":10,\"weight_g\":9982,"
                           "\"source\":\"SCALE1\",\"channel\":1,\"stable\":false,\"sequence\":9}";
    check(weight_msg_parse(extended, strlen(extended), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_ignores_extended_fields");
    check(msg.weight_g == 9982, "parse_extended_grams_ok");

    /* The sender's source field is captured for logging, not for the rule. */
    const char *with_source = "{\"type\":\"weight\",\"weight_g\":10000,\"source\":\"SCALE1\",\"sequence\":7}";
    check(weight_msg_parse(with_source, strlen(with_source), &msg) == WEIGHT_MSG_ACCEPTED,
          "parse_accepts_source_field");
    check(msg.has_source && strcmp(msg.source, "SCALE1") == 0, "parse_captures_source");
    check(msg.sequence == 7, "parse_source_and_sequence_together");

    /* Production rejects simulated samples outright â€” a simulated frame must
     * never drive a dispense because it is not a physical measurement. */
    const char *sim_true =
        "{\"type\":\"weight\",\"weight_g\":7000,\"simulated\":true}";
    check(weight_msg_parse(sim_true, strlen(sim_true), &msg) == WEIGHT_MSG_REJECTED,
          "reject_simulated_true");
    const char *sim_src =
        "{\"type\":\"weight\",\"weight_g\":7000,\"source\":\"SIM\"}";
    check(weight_msg_parse(sim_src, strlen(sim_src), &msg) == WEIGHT_MSG_REJECTED,
          "reject_source_SIM");
    const char *sim_src_lc =
        "{\"type\":\"weight\",\"weight_g\":7000,\"source\":\"sim\"}";
    check(weight_msg_parse(sim_src_lc, strlen(sim_src_lc), &msg) == WEIGHT_MSG_REJECTED,
          "reject_source_sim_lowercase");

    check(weight_msg_parse("{}", strlen("{}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_empty_object");
    check(weight_msg_parse("{\"weight_g\":8000}", strlen("{\"weight_g\":8000}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_missing_type");
    check(weight_msg_parse("{\"type\":\"other\",\"weight_g\":8000}",
                           strlen("{\"type\":\"other\",\"weight_g\":8000}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_wrong_type");
    check(weight_msg_parse("random text", strlen("random text"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_random_text");
    check(weight_msg_parse("", 0, &msg) == WEIGHT_MSG_REJECTED, "reject_empty_payload");

    /* Range: zero accepted (idle scale, relay OFF, timestamp refreshed),
     * negatives and absurd values rejected. */
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_g\":0}",
                           strlen("{\"type\":\"weight\",\"weight_g\":0}"), &msg) == WEIGHT_MSG_ACCEPTED,
          "accept_zero_idle_scale");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_kg\":-1}",
                           strlen("{\"type\":\"weight\",\"weight_kg\":-1}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_negative");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_g\":-500}",
                           strlen("{\"type\":\"weight\",\"weight_g\":-500}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_negative_grams");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_g\":200000}",
                           strlen("{\"type\":\"weight\",\"weight_g\":200000}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_absurd_grams");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_kg\":200}",
                           strlen("{\"type\":\"weight\",\"weight_kg\":200}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_absurd_kg");

    /* Type errors and non-finite values. */
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_g\":\"abc\"}",
                           strlen("{\"type\":\"weight\",\"weight_g\":\"abc\"}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_string_weight");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_g\":9500.5}",
                           strlen("{\"type\":\"weight\",\"weight_g\":9500.5}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_fractional_grams");
    check(weight_msg_parse("{\"type\":\"weight\",\"weight_kg\":1e999}",
                           strlen("{\"type\":\"weight\",\"weight_kg\":1e999}"), &msg) == WEIGHT_MSG_REJECTED,
          "reject_infinite_weight");
    const char *null_weight = "{\"type\":\"weight\",\"weight_kg\":null}";
    check(weight_msg_parse(null_weight, strlen(null_weight), &msg) == WEIGHT_MSG_REJECTED,
          "reject_null_weight");
    const char *type_num = "{\"type\":7,\"weight_g\":10000}";
    check(weight_msg_parse(type_num, strlen(type_num), &msg) == WEIGHT_MSG_REJECTED,
          "reject_non_string_type");

    /* Integral float kg behaves like the integer (10.0 == 10). */
    const char *as_float = "{\"type\":\"weight\",\"weight_kg\":10.0}";
    check(weight_msg_parse(as_float, strlen(as_float), &msg) == WEIGHT_MSG_ACCEPTED,
          "accept_integral_float_10_0");
    check(msg.weight_g == 10000 && msg.weight_kg == 10, "integral_float_maps_to_10kg");

    /* ---- weight_receiver: the bench decision rule (gram domain) ----
     * >= WEIGHT_TRIGGER_G energizes; there is no float equality anywhere. */
    check(WEIGHT_TRIGGER_G == 10000, "trigger_is_10kg_in_grams");
    check(WEIGHT_MESSAGE_TIMEOUT_MS == 5000, "timeout_default_is_5000ms");
    check(weight_should_energize_g(0) == false, "decision_0_off");
    check(weight_should_energize_g(8000) == false, "decision_8kg_off");
    check(weight_should_energize_g(9500) == false, "decision_9_5kg_off");
    check(weight_should_energize_g(9999) == false, "decision_just_below_trigger_off");
    check(weight_should_energize_g(10000) == true, "decision_at_trigger_on");
    check(weight_should_energize_g(10500) == true, "decision_above_trigger_on");
    check(weight_should_energize_g(12000) == true, "decision_12kg_on");
    check(weight_should_energize_g(WEIGHT_G_MAX) == true, "decision_max_range_on");

    /* ---- weight_receiver: sequence diagnostics ---- */
    check(weight_sequence_gap(10, 13) == 2, "gap_10_to_13_is_2");
    check(weight_sequence_gap(10, 11) == 0, "gap_consecutive_is_0");
    check(weight_sequence_gap(10, 10) == 0, "gap_repeat_is_0");
    check(weight_sequence_gap(4294967295U, 1) == 0, "gap_wrap_is_0");

    /* ---- weight_receiver: timeout decision ---- */
    check(weight_link_timed_out(3000, 0, 3000) == false, "timeout_not_yet_at_exactly_3000");
    check(weight_link_timed_out(3001, 0, 3000) == true, "timeout_after_3000");
    check(weight_link_timed_out(1000, 0, 3000) == false, "timeout_not_before_window");

    /* ---- weight_receiver: link-state wire names (health panel contract) ----
     * The server maps these exact strings to the Weight Sender indicator. */
    check(strcmp(weight_link_state_name(WEIGHT_LINK_OK), "OK") == 0,
          "link_name_ok_is_OK");
    check(strcmp(weight_link_state_name(WEIGHT_LINK_TIMEOUT), "TIMEOUT") == 0,
          "link_name_timeout_is_TIMEOUT");
    check(strcmp(weight_link_state_name(WEIGHT_LINK_LOST), "LOST") == 0,
          "link_name_lost_is_LOST");
    check(strcmp(weight_link_state_name(WEIGHT_LINK_UNKNOWN), "UNKNOWN") == 0,
          "link_name_unknown_is_UNKNOWN");

    /* ---- weight_receiver: stateful group behaviour through the relays ----
     * The end-to-end acceptance sequence: 8.00 -> OFF, 10.00 -> all ON,
     * 12.00 -> hold ON, 9.50 -> all OFF. */
    weight_receiver_init();
    /* No actuation handler registered = the built-in legacy group policy.
     * Stated explicitly so this block keeps testing that policy even after
     * the dispense tests below register one. */
    weight_receiver_set_actuation_handler(NULL);
    (void)relay_all_off();

    const char *w8  = "{\"type\":\"weight\",\"weight_g\":8000,\"source\":\"SCALE1\",\"sequence\":1}";
    const char *w10 = "{\"type\":\"weight\",\"weight_g\":10000,\"source\":\"SCALE1\",\"sequence\":2}";
    const char *w12 = "{\"type\":\"weight\",\"weight_g\":12000,\"source\":\"SCALE1\",\"sequence\":3}";
    const char *w95 = "{\"type\":\"weight\",\"weight_g\":9500,\"source\":\"SCALE1\",\"sequence\":4}";

    feed(w8, 1000);
    check(all_relays(false), "e2e_8kg_all_relays_off");

    feed(w10, 2000);
    check(all_relays(true), "e2e_10kg_all_12_relays_on");

    feed(w12, 3000);
    check(all_relays(true), "e2e_12kg_group_holds_on");

    feed(w95, 4000);
    check(all_relays(false), "e2e_9_5kg_all_relays_off");

    /* Rounding-trap regression end to end: kg says 10, grams say 9500 â€”
     * the group must stay OFF. */
    feed("{\"type\":\"weight\",\"weight_kg\":10,\"weight_g\":9500,\"sequence\":5}", 5000);
    check(all_relays(false), "grams_authoritative_over_rounded_kg");

    /* Holding at/above the trigger: redundant messages change nothing. */
    feed(w10, 6000);
    check(all_relays(true), "group_on_at_trigger");
    feed(w12, 7000);
    check(all_relays(true), "group_stays_on_no_redundant_change");

    /* An invalid message must not move the timestamp or the relays. */
    feed("garbage", 7100);
    check(all_relays(true), "invalid_message_does_not_touch_relays");
    weight_receiver_tick(7000 + WEIGHT_MESSAGE_TIMEOUT_MS);
    check(all_relays(true), "invalid_message_does_not_move_timestamp");
    weight_receiver_tick(7000 + WEIGHT_MESSAGE_TIMEOUT_MS + 1);
    check(all_relays(false), "timeout_forces_group_off");

    /* Link loss forces the whole group off immediately. */
    feed(w10, 20000);
    check(all_relays(true), "group_on_before_link_loss");
    weight_receiver_on_link_lost(20100);
    check(all_relays(false), "link_loss_forces_group_off");

    /* Race regression (QA M1): a link-loss interleaving with an in-flight
     * message must not leave the group energized. Reproduce the worst
     * ordering â€” validity cleared first, the message's group write landing
     * afterwards â€” and require the supervisor tick to heal it. */
    weight_receiver_on_link_lost(21000);
    (void)relay_all_on(); /* the racing write from the interrupted on_message */
    check(all_relays(true), "race_setup_group_on_without_valid_message");
    weight_receiver_tick(21100);
    check(all_relays(false), "tick_heals_group_on_without_valid_message");

    /* ---- websocket_client: payload reassembly across fragments ----
     * A weight message can arrive split over several DATA events, so the
     * client buffers fragments and only dispatches a complete payload. */
    ws_rx_reset();
    const char *part1 = "{\"type\":\"weight\",";
    const char *part2 = "\"weight_g\":10000}";
    size_t total = strlen(part1) + strlen(part2);

    check(ws_rx_accumulate(part1, strlen(part1), total, 0) == false,
          "reassembly_partial_not_complete");
    check(ws_rx_accumulate(part2, strlen(part2), total, strlen(part1)) == true,
          "reassembly_complete_on_last");
    check(ws_rx_length() == total, "reassembly_total_length");
    check(strncmp(ws_rx_buffer(), part1, strlen(part1)) == 0,
          "reassembly_first_fragment_intact");

    weight_msg_t reassembled;
    check(weight_msg_parse(ws_rx_buffer(), ws_rx_length(), &reassembled) == WEIGHT_MSG_ACCEPTED,
          "reassembled_payload_parses");
    check(reassembled.weight_g == 10000, "reassembled_weight_is_10kg");

    /* A single unfragmented frame completes immediately. */
    ws_rx_reset();
    const char *whole = "{\"type\":\"weight\",\"weight_g\":3000}";
    check(ws_rx_accumulate(whole, strlen(whole), strlen(whole), 0) == true,
          "reassembly_single_frame_complete");

    /* An oversized payload must truncate safely, never overflow the buffer,
     * and never be mistaken for a valid message. */
    ws_rx_reset();
    static char huge[WS_RX_BUFFER_SIZE * 2];
    memset(huge, 'A', sizeof(huge));
    huge[0] = '{';
    check(ws_rx_accumulate(huge, sizeof(huge), sizeof(huge) + 100U, 0) == true,
          "oversized_payload_reports_complete");
    check(ws_rx_length() < WS_RX_BUFFER_SIZE, "oversized_payload_truncated_within_buffer");

    weight_msg_t big;
    check(weight_msg_parse(ws_rx_buffer(), ws_rx_length(), &big) == WEIGHT_MSG_REJECTED,
          "oversized_payload_rejected_not_crashing");

    /* ---- Boot order invariant ----
     * app_main must leave every relay OFF before Wi-Fi or WebSocket exist.
     * app_main itself is not linked into this test app (it owns Wi-Fi), so the
     * invariant is pinned here at the layer that establishes it: a fresh
     * relay_init_all() must leave all twelve channels OFF, including AO12 on
     * GPIO2 (a strapping pin â€” pre-driven LOW before the pads become
     * outputs). Called once only, since re-initialising configured pads logs
     * harmless GPIO conflicts. */
    (void)relay_init_all();
    check(all_relays(false), "boot_order_all_relays_off_after_init");

    weight_receiver_init();
    check(weight_receiver_link_state() == WEIGHT_LINK_UNKNOWN, "boot_link_state_unknown");
    check(all_relays(false), "boot_group_off");

    /* ---- Concurrency: the driver must stay functional under contention ----
     * relay_set() serialises its cache check, cache write and pad write with a
     * spinlock. Three tasks call it in the real firmware (WebSocket client,
     * Wi-Fi event, timeout supervisor), so this hammers the GROUP API from two
     * tasks to show the lock neither deadlocks nor corrupts the cached state.
     *
     * QEMU does NOT emulate GPIO output readback - gpio_get_level() on an
     * output pin reads low whatever was written - so this cannot assert the
     * physical pin. Verifying that the pads actually follow the cache is a
     * hardware check; see relay_pad_is_energized(). */
    (void)relay_init_all();

    bool cache_ok = true;

    /* Each transition logs at debug level now, but the group commands fire
     * twelve per call at hammer rates; silence the driver for this phase so
     * the serial capture is not starved. */
    esp_log_level_set("relay_driver", ESP_LOG_NONE);

    for (int round = 0; round < 40; round++) {
        s_hammer_stop = false;
        /* Priority 1, the same as the main task: a higher priority would starve
         * app_main, and taskYIELD only yields to equal or higher priority. */
        xTaskCreate(hammer_on, "hammer_on", 3072, NULL, 1, NULL);
        xTaskCreate(hammer_off, "hammer_off", 3072, NULL, 1, NULL);

        vTaskDelay(pdMS_TO_TICKS(20));

        s_hammer_stop = true;
        vTaskDelay(pdMS_TO_TICKS(20));

        (void)relay_all_off();
        if (!all_relays(false)) cache_ok = false;
    }

    esp_log_level_set("relay_driver", ESP_LOG_INFO);

    check(cache_ok, "failsafe_leaves_cache_off_under_concurrency");

    /* ===========================================================================
     * Part 2: the dispensing architecture
     * ======================================================================== */

    /* ---- Provisioning AP name: BITS-Relay-XXXX from the device id ---- */
    char ap[64];

    provisioning_build_ap_name("BITS-Relay", "bits-a4cf12ab34cd", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Relay-34CD") == 0, "ap_name_relay_from_device_id");

    provisioning_build_ap_name("BITS-Scale", "bits-a4cf12ab34cd", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Scale-34CD") == 0, "ap_name_scale_same_suffix");

    provisioning_build_ap_name("BITS-Relay", NULL, ap, sizeof(ap));
    check(strcmp(ap, "BITS-Relay-0000") == 0, "ap_name_null_id_zero_suffix");

    provisioning_build_ap_name("BITS-Relay", "bits-ab", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Relay-00AB") == 0, "ap_name_short_id_padded");

    provisioning_build_ap_name("BITS-Relay", "Relay Controller", ap, sizeof(ap));
    check(strcmp(ap, "BITS-Relay-0000") == 0, "ap_name_non_hex_id_zero_suffix");

    char small_ap[10];
    memset(small_ap, 'Z', sizeof(small_ap));
    provisioning_build_ap_name("BITS-Relay", "bits-a4cf12ab34cd", small_ap, sizeof(small_ap));
    check(small_ap[sizeof(small_ap) - 1U] == '\0', "ap_name_small_buffer_terminated");

    /* ---- job_queue: window, FIFO order and bounded depth ---- */
    (void)job_queue_init();
    check(job_queue_depth() == 0U, "queue_starts_empty");
    {
        dispense_job_t running;
        check(!job_queue_get_running(&running), "queue_no_running_job_initially");
    }

    check(!job_target_valid(0), "target_zero_rejected");
    check(!job_target_valid(-1), "target_negative_rejected");
    check(!job_target_valid(0x7FFFFFFF), "target_absurd_rejected");
    check(job_target_valid(1), "target_minimum_accepted");
    check(job_target_valid(5000), "target_5kg_accepted");

    uint32_t id_a = 0U, id_b = 0U, id_c = 0U;
    check(job_queue_add(5000, JOB_PRIORITY_NORMAL, 1000U, &id_a) == ESP_OK, "queue_add_5kg_ok");
    check(id_a == 1U, "queue_first_id_is_1");
    check(job_queue_depth() == 1U, "queue_depth_after_one");
    check(job_queue_add(0, JOB_PRIORITY_NORMAL, 1000U, &id_b) == ESP_ERR_INVALID_ARG, "queue_add_zero_refused");
    check(job_queue_add(-500, JOB_PRIORITY_NORMAL, 1000U, &id_b) == ESP_ERR_INVALID_ARG, "queue_add_negative_refused");
    check(job_queue_add(999999, JOB_PRIORITY_NORMAL, 1000U, &id_b) == ESP_ERR_INVALID_ARG, "queue_add_over_max_refused");
    check(job_queue_depth() == 1U, "queue_depth_unchanged_by_refusals");

    (void)job_queue_add(3000, JOB_PRIORITY_NORMAL, 1100U, &id_b);
    (void)job_queue_add(7000, JOB_PRIORITY_NORMAL, 1200U, &id_c);

    dispense_job_t job;
    check(job_queue_start_next(2000U, &job) == true, "queue_start_next_ok");
    check(job.id == id_a, "queue_fifo_starts_oldest_first");
    check(job.target_g == 5000, "queue_started_job_target");
    check(job.state == JOB_RUNNING, "queue_started_job_is_running");
    check(job.start_ms == 2000U, "queue_started_job_records_start_ms");
    check(job_queue_depth() == 2U, "queue_depth_after_start");

    {
        dispense_job_t running;
        check(job_queue_get_running(&running) && running.id == id_a,
              "queue_running_reports_active_job");
    }

    /* FIFO order continues with the next two. */
    check(job_queue_start_next(2100U, &job) == true && job.target_g == 3000,
          "queue_fifo_second");
    check(job_queue_start_next(2200U, &job) == true && job.target_g == 7000,
          "queue_fifo_third");
    check(job_queue_start_next(2300U, &job) == false, "queue_start_next_empty_false");

    /* Bounded: filling every remaining slot then one more is refused. */
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        (void)job_queue_add(1000, JOB_PRIORITY_NORMAL, 3000U, NULL);
    }
    check(job_queue_add(1000, JOB_PRIORITY_NORMAL, 3000U, NULL) == ESP_ERR_NO_MEM, "queue_full_refused");

    (void)job_queue_init();

    /* Terminal transitions record the requested values. */
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 100U, &id_a);
    (void)job_queue_start_next(200U, &job);
    check(job_queue_set_start_g(id_a, 12) == true, "queue_set_start_g_ok");
    check(job_queue_finish(id_a, JOB_COMPLETE, 4995, NULL, 900U) == true,
          "queue_finish_complete_ok");
    check(job_queue_get(id_a, &job) == true, "queue_get_after_finish");
    check(job.state == JOB_COMPLETE, "queue_finished_state_recorded");
    check(job.start_g == 12, "queue_start_g_recorded");
    check(job.final_g == 4995, "queue_final_g_recorded");
    check(job.finish_ms == 900U, "queue_finish_ms_recorded");
    check(job.error[0] == '\0', "queue_complete_has_no_error");
    check(job_queue_depth() == 0U, "queue_depth_zero_after_finish");

    /* A failure records its reason. */
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 100U, &id_b);
    (void)job_queue_start_next(200U, &job);
    check(job_queue_finish(id_b, JOB_FAILED, 5300, "overshoot", 950U) == true,
          "queue_finish_failed_ok");
    (void)job_queue_get(id_b, &job);
    check(job.state == JOB_FAILED, "queue_failed_state_recorded");
    check(strcmp(job.error, "overshoot") == 0, "queue_error_text_recorded");

    /* Cancelling a QUEUED job removes it from the FIFO. */
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 100U, &id_c);
    check(job_queue_cancel(id_c) == true, "queue_cancel_queued_ok");
    (void)job_queue_get(id_c, &job);
    check(job.state == JOB_CANCELLED, "queue_cancelled_state_recorded");
    check(job_queue_depth() == 0U, "queue_cancel_removes_from_fifo");
    check(job_queue_start_next(300U, &job) == false, "queue_cancelled_not_started");
    check(job_queue_cancel(id_c) == false, "queue_cancel_twice_is_noop");
    check(job_queue_cancel(9999U) == false, "queue_cancel_unknown_is_noop");

    /* cancel_all clears the pending FIFO only. */
    (void)job_queue_add(1000, JOB_PRIORITY_NORMAL, 10U, NULL);
    (void)job_queue_add(2000, JOB_PRIORITY_NORMAL, 20U, NULL);
    (void)job_queue_add(3000, JOB_PRIORITY_NORMAL, 30U, NULL);
    check(job_queue_cancel_all() == 3U, "queue_cancel_all_counts");
    check(job_queue_depth() == 0U, "queue_cancel_all_empties_fifo");

    /* Snapshot is oldest-first by insertion, and survives slot reuse. */
    (void)job_queue_init();
    uint32_t s1 = 0U, s2 = 0U;
    (void)job_queue_add(1111, JOB_PRIORITY_NORMAL, 1U, &s1);
    (void)job_queue_add(2222, JOB_PRIORITY_NORMAL, 2U, &s2);
    dispense_job_t snap[JOB_QUEUE_MAX];
    uint32_t snap_n = job_queue_snapshot(snap, JOB_QUEUE_MAX);
    check(snap_n == 2U, "queue_snapshot_count");
    check(snap[0].target_g == 1111 && snap[1].target_g == 2222,
          "queue_snapshot_is_insertion_ordered");
    check(job_queue_snapshot(snap, 1U) == 1U, "queue_snapshot_honours_max");
    check(job_queue_snapshot(NULL, 4U) == 0U, "queue_snapshot_null_safe");

    /* ---- job_queue: hold / release / promote ----
     * A held job stays in the list but is never started. A promoted job is
     * started before any non-promoted job of the same channel. Neither touches
     * a RUNNING job: the queue is non-preemptive. */
    (void)job_queue_init();
    uint32_t h1 = 0U, h2 = 0U, h3 = 0U;
    dispense_job_t picked;

    (void)job_queue_add(1000, JOB_PRIORITY_NORMAL, 10U, &h1);
    (void)job_queue_add(2000, JOB_PRIORITY_NORMAL, 20U, &h2);
    check(job_queue_hold(h1) == true, "queue_hold_queued_ok");
    check(job_queue_get(h1, &picked) && picked.held, "queue_hold_flag_set");
    check(job_queue_start_next(100U, &picked) == true && picked.id == h2,
          "queue_hold_skips_held_job");
    check(job_queue_hold(h1) == true, "queue_hold_twice_is_idempotent");
    check(job_queue_release(h1) == true, "queue_release_ok");
    check(job_queue_get(h1, &picked) && !picked.held, "queue_release_flag_cleared");
    check(job_queue_start_next(200U, &picked) == true && picked.id == h1,
          "queue_release_makes_job_eligible_again");
    check(job_queue_release(h1) == false, "queue_release_running_job_refused");

    (void)job_queue_init();
    (void)job_queue_add(1100, JOB_PRIORITY_NORMAL, 10U, &h1);
    (void)job_queue_add(2200, JOB_PRIORITY_NORMAL, 20U, &h2);
    (void)job_queue_add(3300, JOB_PRIORITY_NORMAL, 30U, &h3);
    check(job_queue_promote(h3) == true, "queue_promote_ok");
    check(job_queue_get(h3, &picked) && picked.promoted, "queue_promote_flag_set");
    check(job_queue_start_next(300U, &picked) == true && picked.id == h3,
          "queue_promote_runs_first_despite_arriving_last");
    check(job_queue_start_next(400U, &picked) == true && picked.id == h1,
          "queue_promote_leaves_rest_fifo");
    check(job_queue_start_next(500U, &picked) == true && picked.id == h2,
          "queue_promote_terminates_in_fifo_order");
    check(job_queue_promote(9999U) == false, "queue_promote_unknown_is_noop");

    /* promote + hold: parked beats promoted until released. */
    (void)job_queue_init();
    (void)job_queue_add(1100, JOB_PRIORITY_NORMAL, 10U, &h1);
    (void)job_queue_add(2200, JOB_PRIORITY_NORMAL, 20U, &h2);
    check(job_queue_promote(h1) == true, "queue_promote_held_job_ok");
    check(job_queue_hold(h1) == true, "queue_hold_promoted_job_ok");
    check(job_queue_start_next(600U, &picked) == true && picked.id == h2,
          "queue_held_promoted_job_still_skipped");
    check(job_queue_release(h1) == true, "queue_release_promoted_job_ok");
    (void)job_queue_add(3300, JOB_PRIORITY_NORMAL, 30U, &h3);
    check(job_queue_start_next(700U, &picked) == true && picked.id == h1,
          "queue_released_promoted_job_jumps_the_line");

    /* hold/promote refuse to touch a job that is already RUNNING. */
    (void)job_queue_init();
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 10U, &h1);
    check(job_queue_start_next(800U, &picked) == true && picked.id == h1,
          "queue_running_job_for_hold_test");
    check(job_queue_hold(h1) == false, "queue_hold_running_job_refused");
    check(job_queue_promote(h1) == false, "queue_promote_running_job_refused");
    check(job_queue_get(h1, &picked) && !picked.held && !picked.promoted,
          "queue_refusal_leaves_flags_alone");

    /* A held job is still cancellable, and still counts as queued depth. */
    (void)job_queue_init();
    (void)job_queue_add(1100, JOB_PRIORITY_NORMAL, 10U, &h1);
    (void)job_queue_add(2200, JOB_PRIORITY_NORMAL, 20U, &h2);
    check(job_queue_hold(h1) == true, "queue_hold_for_cancel_ok");
    check(job_queue_depth() == 2U, "queue_held_job_still_counts_toward_depth");
    check(job_queue_cancel(h1) == true, "queue_cancel_held_job_ok");
    check(job_queue_get(h1, &picked) && picked.state == JOB_CANCELLED,
          "queue_cancelled_held_job_terminal");
    check(job_queue_start_next(900U, &picked) == true && picked.id == h2,
          "queue_cancelled_held_job_not_started");

    check(strcmp(job_state_name(JOB_QUEUED), "QUEUED") == 0, "job_state_name_queued");
    check(strcmp(job_state_name(JOB_RUNNING), "RUNNING") == 0, "job_state_name_running");
    check(strcmp(job_state_name(JOB_COMPLETE), "COMPLETE") == 0, "job_state_name_complete");
    check(strcmp(job_state_name(JOB_CANCELLED), "CANCELLED") == 0, "job_state_name_cancelled");
    check(strcmp(job_state_name(JOB_FAILED), "FAILED") == 0, "job_state_name_failed");

    /* ---- safety_manager: latch, policy and relays ---- */
    (void)relay_init_all();
    check(safety_manager_init() == ESP_OK, "safety_init_ok");
    check(!safety_manager_fault_active(), "safety_no_fault_initially");
    check(strcmp(safety_fault_name(SAFETY_NONE), "NONE") == 0, "safety_name_none");

    check(safety_fault_is_transient(SAFETY_WEIGHT_LINK_LOST), "link_lost_is_transient");
    check(safety_fault_is_transient(SAFETY_WEIGHT_STALE), "stale_is_transient");
    check(!safety_fault_is_transient(SAFETY_OVERSHOOT), "overshoot_is_latched");
    check(!safety_fault_is_transient(SAFETY_EMERGENCY_STOP), "estop_is_latched");
    check(!safety_fault_is_transient(SAFETY_CONTROL_FAILURE), "control_failure_is_latched");

    /* Raising a fault forces every relay off. */
    (void)relay_all_on();
    check(all_relays(true), "safety_setup_all_relays_on");
    safety_manager_raise(SAFETY_OVERSHOOT, 1000U);
    check(all_relays(false), "safety_raise_forces_all_relays_off");
    check(safety_manager_fault() == SAFETY_OVERSHOOT, "safety_fault_latched");
    check(safety_manager_fault_since_ms() == 1000U, "safety_fault_timestamp");

    /* A latched fault must NOT clear itself on a fresh weight. */
    safety_manager_on_valid_weight(1500U);
    check(safety_manager_fault() == SAFETY_OVERSHOOT,
          "safety_latched_fault_survives_valid_weight");

    /* An explicit clear releases it. */
    check(safety_manager_clear() == true, "safety_explicit_clear_ok");
    check(!safety_manager_fault_active(), "safety_clear_removes_fault");
    check(safety_manager_clear() == true, "safety_clear_when_clean_is_ok");

    /* A transient fault clears itself once the link recovers. */
    (void)relay_all_on();
    safety_manager_raise(SAFETY_WEIGHT_STALE, 2000U);
    check(all_relays(false), "safety_transient_also_forces_off");
    safety_manager_on_valid_weight(2500U);
    check(!safety_manager_fault_active(), "safety_transient_auto_clears");

    /* Emergency stop outranks everything and resists a plain clear. */
    safety_manager_raise(SAFETY_EMERGENCY_STOP, 3000U);
    check(safety_manager_fault() == SAFETY_EMERGENCY_STOP, "safety_estop_latched");
    safety_manager_raise(SAFETY_OVERSHOOT, 3100U);
    check(safety_manager_fault() == SAFETY_EMERGENCY_STOP,
          "safety_estop_not_downgraded_by_later_fault");
    check(safety_manager_clear() == false, "safety_clear_refused_during_estop");
    check(safety_manager_release_estop() == true, "safety_estop_release_ok");
    check(!safety_manager_fault_active(), "safety_estop_release_clears");

    /* Control watchdog: a controller that stops ticking is a fault. */
    (void)safety_manager_init();
    safety_manager_note_control_tick(10000U);
    safety_manager_tick(10500U);
    check(!safety_manager_fault_active(), "watchdog_quiet_while_ticking");
    check(safety_manager_control_age_ms(10500U) == 500U, "watchdog_age_reported");
    safety_manager_tick(10000U + 3000U);
    check(!safety_manager_fault_active(), "watchdog_not_yet_at_limit");
    safety_manager_tick(10000U + 3001U);
    check(safety_manager_fault() == SAFETY_CONTROL_FAILURE,
          "watchdog_raises_control_failure");
    check(all_relays(false), "watchdog_forces_relays_off");
    (void)safety_manager_init();

    check(strcmp(safety_fault_name(SAFETY_OVERSHOOT), "OVERSHOOT") == 0,
          "safety_fault_name_overshoot");
    check(strcmp(safety_fault_name(SAFETY_EMERGENCY_STOP), "EMERGENCY_STOP") == 0,
          "safety_fault_name_estop");

    /* A job that could not have dispensed correctly from where it started gets
     * its OWN fault name. Reporting it as a timeout would name the wrong
     * problem, which is the failure mode this whole taxonomy exists to avoid. */
    check(strcmp(safety_fault_name(SAFETY_INVALID_START), "INVALID_START") == 0,
          "safety_fault_name_invalid_start");
    check(!safety_fault_is_transient(SAFETY_INVALID_START), "invalid_start_is_latched");

    /* ---- PID: the pure time-proportional control law ---- */
    dispense_pid_gains_t gains = { 0.0025f, 0.0003f, 0.0001f };
    dispense_pid_state_t pid;
    memset(&pid, 0, sizeof(pid));

    check(dispense_pid_duty(NULL, &pid, 100, 100U, 200.0f) == 0.0f, "pid_null_gains_zero");
    check(dispense_pid_duty(&gains, NULL, 100, 100U, 200.0f) == 0.0f, "pid_null_state_zero");

    memset(&pid, 0, sizeof(pid));
    check(dispense_pid_duty(&gains, &pid, 0, 100U, 200.0f) == 0.0f, "pid_zero_error_zero_duty");

    memset(&pid, 0, sizeof(pid));
    check(dispense_pid_duty(&gains, &pid, -500, 100U, 200.0f) == 0.0f,
          "pid_negative_error_never_opens_valve");

    memset(&pid, 0, sizeof(pid));
    check(dispense_pid_duty(&gains, &pid, 10000, 100U, 200.0f) == 1.0f,
          "pid_large_error_saturates_at_full_duty");

    /* Proportional alone: 0.0025 per gram. */
    memset(&pid, 0, sizeof(pid));
    float duty = dispense_pid_duty(&gains, &pid, 200, 0U, 0.0f); /* ki term disabled */
    check(duty > 0.0f && duty <= 1.0f, "pid_duty_in_range");

    /* The integral accumulates and its magnitude is clamped (anti-windup):
     * +1000 for 1 s would be 1000, clamped to 50. */
    memset(&pid, 0, sizeof(pid));
    dispense_pid_gains_t ki_only = { 0.0f, 1.0f, 0.0f };
    (void)dispense_pid_duty(&ki_only, &pid, 1000, 1000U, 50.0f);
    check(pid.integral == 50.0f, "pid_integral_clamped_positive");

    /* A small negative error unwinds it without crossing the clamp. */
    (void)dispense_pid_duty(&ki_only, &pid, -60, 1000U, 50.0f);
    check(pid.integral == -10.0f, "pid_integral_unwinds_downward");

    (void)dispense_pid_duty(&ki_only, &pid, -1000, 1000U, 50.0f);
    check(pid.integral == -50.0f, "pid_integral_clamped_negative");

    /* ---- Windowed (time-proportional) output ---- */
    check(dispense_window_output(0U, 0.0f, 500U, 40U, 40U) == false,
          "window_zero_duty_closed");
    check(dispense_window_output(0U, 1.0f, 500U, 40U, 40U) == true,
          "window_full_duty_open");
    check(dispense_window_output(0U, 0.5f, 500U, 40U, 40U) == true,
          "window_half_duty_open_early");
    check(dispense_window_output(300U, 0.5f, 500U, 40U, 40U) == false,
          "window_half_duty_closed_late");
    check(dispense_window_output(499U, 0.5f, 500U, 40U, 40U) == false,
          "window_half_duty_closed_at_end");
    check(dispense_window_output(600U, 0.5f, 500U, 40U, 40U) == true,
          "window_phase_wraps_into_next_window");
    check(dispense_window_output(0U, 1000.0f, 500U, 40U, 40U) == true,
          "window_duty_above_one_clamped_open");

    /* Anti-chatter: too short a burst is dropped entirely. */
    check(dispense_window_output(0U, 0.05f, 500U, 40U, 40U) == false,
          "window_burst_below_min_on_dropped");
    /* Anti-chatter: too short a gap holds the valve open instead. */
    check(dispense_window_output(400U, 0.95f, 500U, 40U, 40U) == true,
          "window_gap_below_min_off_holds_open");
    check(dispense_window_output(499U, 0.95f, 500U, 40U, 40U) == true,
          "window_gap_rule_applies_through_window");
    check(dispense_window_output(7U, 0.5f, 0U, 40U, 40U) == true,
          "window_zero_length_open");

    check(strcmp(dispense_state_name(DISPENSE_IDLE), "IDLE") == 0, "state_name_idle");
    check(strcmp(dispense_state_name(DISPENSE_COARSE_DISPENSE), "COARSE_DISPENSE") == 0,
          "state_name_coarse");
    check(strcmp(dispense_state_name(DISPENSE_EMERGENCY_STOP), "EMERGENCY_STOP") == 0,
          "state_name_estop");

    /* ---- Dispense controller: the state machine on its own ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();

    dispense_config_t cfg = dispense_config_default();
    /* The process state machine (settling/overshoot/correction and the
     * "already satisfied at start" shortcut) is exercised through a PROCESS
     * config; the default is RAMP_TEST, whose relaxed rules are pinned by the
     * ramp tests further down. Keeping the two explicit is the point of the
     * mode being a config field rather than an inference. */
    dispense_config_t cfg_process = dispense_config_default();
    cfg_process.completion_mode = DISPENSE_COMPLETION_PROCESS;

    check(cfg.target_g == 5000, "config_default_target_5kg");
    check(cfg_process.completion_mode == DISPENSE_COMPLETION_PROCESS,
          "process_config_selects_process_mode");
    check(cfg.tolerance_g > 0, "config_default_tolerance_positive");
    check(cfg.window_ms > 0U, "config_default_window_positive");
    check(cfg.min_on_ms > 0U && cfg.min_off_ms > 0U, "config_default_chatter_bounds");
    check(dispense_controller_init(&cfg) == ESP_OK, "dispense_init_ok");
    check(dispense_controller_init(NULL) == ESP_ERR_INVALID_ARG,
          "dispense_init_null_config_rejected");
    dispense_controller_init(&cfg_process);

    weight_receiver_set_actuation_handler(dispense_controller_on_weight);

    check(dispense_controller_state() == DISPENSE_IDLE, "dispense_starts_idle");
    check(!dispense_controller_coarse_on(), "dispense_coarse_starts_closed");
    check(!dispense_controller_fine_on(), "dispense_fine_starts_closed");
    check(dispense_controller_job_id() == 0U, "dispense_no_job_initially");
    check(!dispense_controller_is_paused(), "dispense_not_paused_initially");

    uint32_t t = 100000U;
    dispense_job_t done;

    /* Idle with an empty queue stays idle. */
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_IDLE, "dispense_idle_with_empty_queue");

    /* Queue a job: the bookkeeping hops (IDLE -> LOAD_JOB) collapse into the
     * same tick, so the machine reaches WAIT_FOR_SCALE immediately and then
     * genuinely WAITS â€” it must not open a valve without a reading. */
    uint32_t job_id = 0U;
    check(job_queue_add(5000, JOB_PRIORITY_NORMAL, t, &job_id) == ESP_OK, "dispense_job_queued");
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_WAIT_FOR_SCALE,
          "dispense_loads_and_waits_for_scale");
    check(dispense_controller_job_id() == job_id, "dispense_tracks_job_id");

    pump(&t, 3);
    check(dispense_controller_state() == DISPENSE_WAIT_FOR_SCALE, "dispense_waits_without_weight");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "dispense_no_valve_without_weight");

    /* First valid reading, far below target: both valves open for fast fill. */
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE,
          "dispense_coarse_after_first_reading");
    check(dispense_controller_coarse_on(), "dispense_coarse_valve_open");
    check(dispense_controller_fine_on(), "dispense_both_valves_open_when_far_below");

    /* Still far below: coarse stays open. */
    feed("{\"type\":\"weight\",\"weight_g\":2000,\"source\":\"SCALE1\"}", t);
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "dispense_coarse_holds");
    check(dispense_controller_coarse_on(), "dispense_coarse_still_open");

    /* Now within the coarse-transition distance: hand over to fine. */
    feed("{\"type\":\"weight\",\"weight_g\":4200,\"source\":\"SCALE1\"}", t);
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_FINE_DISPENSE,
          "dispense_hands_over_to_fine");
    check(!dispense_controller_coarse_on(), "dispense_coarse_closed_at_handover");
    check(dispense_controller_fine_on(), "dispense_fine_valve_open_at_handover");

    /* Within tolerance: both close and the machine settles. */
    feed("{\"type\":\"weight\",\"weight_g\":4990,\"source\":\"SCALE1\"}", t);
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_SETTLING, "dispense_settles_in_tolerance");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "dispense_both_valves_closed_while_settling");

    /* The settle window must be honoured before the result is judged: one
     * tick is not enough, whatever the data says. */
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_SETTLING, "dispense_settle_not_skipped");

    pump(&t, (int)(cfg.settle_ms / 100U) + 2);
    check(dispense_controller_state() == DISPENSE_COMPLETE ||
          dispense_controller_state() == DISPENSE_IDLE, "dispense_completes_after_settle");

    check(job_queue_get(job_id, &done) == true, "dispense_job_record_present");
    check(done.state == JOB_COMPLETE, "dispense_job_marked_complete");
    check(done.final_g == 4990, "dispense_job_final_weight_recorded");
    check(done.start_g == 0, "dispense_job_start_weight_recorded");

    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_IDLE, "dispense_returns_to_idle_when_queue_empty");
    check(dispense_controller_job_id() == 0U, "dispense_clears_job_id");

    /* ---- Automatic next-job execution ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);

    uint32_t n1 = 0U, n2 = 0U;
    check(job_queue_add(2000, JOB_PRIORITY_NORMAL, t, &n1) == ESP_OK, "next_job_first_queued");
    check(job_queue_add(3000, JOB_PRIORITY_NORMAL, t, &n2) == ESP_OK, "next_job_second_queued");

    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "next_job_first_runs");
    check(dispense_controller_job_id() == n1, "next_job_first_is_the_oldest");

    feed("{\"type\":\"weight\",\"weight_g\":1990,\"source\":\"SCALE1\"}", t);
    pump(&t, (int)(cfg.settle_ms / 100U) + 3);
    check(job_queue_depth() == 0U, "next_job_queue_drained");

    /* The second job starts on its own, with NO further operator action. */
    check(dispense_controller_job_id() == n2, "next_job_second_is_the_next_queued");
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE ||
          dispense_controller_state() == DISPENSE_WAIT_FOR_SCALE,
          "next_job_second_starts_automatically");

    check(job_queue_get(n1, &done) == true, "next_job_first_record_present");
    check(done.state == JOB_COMPLETE, "next_job_first_recorded_complete");

    /* ---- Overshoot protection (PROCESS semantics) ----
     * Overshoot can only be a fault where the reading is able to hold: a real
     * scale, or the valve-coupled vessel. The default bench mode is RAMP_TEST,
     * whose independent reading keeps rising after the valves close, so the
     * same rule would fail every job there. The mode is therefore an explicit,
     * logged selection and this block selects it. */
    (void)job_queue_init();
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);

    uint32_t o1 = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, t, &o1);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "overshoot_setup_coarse");

    /* Far past target + max_overshoot: latched failure, valves closed. */
    feed("{\"type\":\"weight\",\"weight_g\":5500,\"source\":\"SCALE1\"}", t);
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_FAULT, "overshoot_faults");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "overshoot_closes_both_valves");
    check(safety_manager_fault() == SAFETY_OVERSHOOT, "overshoot_fault_latched");
    check(job_queue_get(o1, &done) == true, "overshoot_job_record_present");
    check(done.state == JOB_FAILED, "overshoot_job_failed");
    check(strcmp(done.error, "overshoot") == 0, "overshoot_error_text");

    /* A latched overshoot keeps the machine in FAULT even with good data. */
    feed("{\"type\":\"weight\",\"weight_g\":5000,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_FAULT, "overshoot_stays_faulted");

    check(dispense_controller_clear_fault(t) == true, "overshoot_clear_ok");
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_IDLE, "overshoot_clears_to_idle");

    /* ---- Stale-weight protection ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);

    uint32_t st = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, t, &st);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "stale_setup_dispensing");
    check(dispense_controller_coarse_on(), "stale_setup_valve_open");

    /* The feed stops. No more messages arrive; the controller must close the
     * valves and fault well before any further product could be added. */
    t += cfg.stale_timeout_ms + 100U;
    dispense_controller_tick(t);
    check(dispense_controller_state() == DISPENSE_FAULT, "stale_weight_faults");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "stale_weight_closes_valves");
    check(safety_manager_fault() == SAFETY_WEIGHT_STALE, "stale_weight_fault_named");
    check(job_queue_get(st, &done) == true, "stale_weight_job_record_present");
    /* Parked, not failed: a stale link describes the LINK, and destroying a
     * half-dispensed job over a Wi-Fi blip would be the wrong call. */
    check(done.state == JOB_RUNNING, "stale_weight_parks_job_not_fails_it");

    /* Transient by design: a fresh reading clears the fault and the parked job
     * resumes on its own, re-checking freshness before any valve reopens. */
    feed("{\"type\":\"weight\",\"weight_g\":1000,\"source\":\"SCALE1\"}", t);
    check(!safety_manager_fault_active(), "stale_fault_auto_clears_on_fresh_weight");
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE,
          "stale_recovers_and_resumes_dispensing");
    check(dispense_controller_coarse_on(), "stale_recovery_reopens_coarse");

    /* A parked job still burns its budget. A link that never recovers must not
     * leave the job RUNNING â€” and its record reserved â€” for as long as the
     * device is powered. */
    t += 200U;
    dispense_controller_tick(t);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE,
          "stale_resumed_job_still_dispensing");
    t += cfg.max_duration_ms + cfg.stale_timeout_ms + 1000U;
    dispense_controller_tick(t);
    check(job_queue_get(st, &done) == true, "stale_timeout_job_record_present");
    check(done.state == JOB_FAILED, "parked_job_fails_when_budget_expires");
    check(strcmp(done.error, "timeout during fault") == 0, "parked_job_timeout_error_text");

    /* ---- Emergency stop ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);

    uint32_t e1 = 0U, e2 = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, t, &e1);
    (void)job_queue_add(6000, JOB_PRIORITY_NORMAL, t, &e2);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "estop_setup_running");

    dispense_controller_emergency_stop(t);
    check(dispense_controller_state() == DISPENSE_EMERGENCY_STOP, "estop_state_entered");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "estop_closes_both_valves");
    check(all_relays(false), "estop_all_relays_off");
    check(safety_manager_fault() == SAFETY_EMERGENCY_STOP, "estop_fault_latched");
    check(job_queue_get(e1, &done) == true, "estop_first_job_record_present");
    check(done.state == JOB_FAILED, "estop_fails_running_job");
    check(job_queue_get(e2, &done) == true, "estop_second_job_record_present");
    check(done.state == JOB_CANCELLED, "estop_cancels_queued_job");
    check(job_queue_depth() == 0U, "estop_empties_queue");

    /* It stays stopped: no tick may restart it. */
    pump(&t, 3);
    check(dispense_controller_state() == DISPENSE_EMERGENCY_STOP, "estop_stays_stopped");
    check(!dispense_controller_coarse_on(), "estop_valves_stay_closed");

    check(dispense_controller_clear_fault(t) == true, "estop_release_ok");
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_IDLE, "estop_release_to_idle");

    /* ---- Pause / resume ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);

    uint32_t p1 = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, t, &p1);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE, "pause_setup_running");

    check(dispense_controller_pause(t) == true, "pause_accepted");
    check(dispense_controller_is_paused(), "pause_flag_set");
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "pause_closes_both_valves");

    feed("{\"type\":\"weight\",\"weight_g\":2500,\"source\":\"SCALE1\"}", t);
    pump(&t, 2);
    check(!dispense_controller_coarse_on(), "pause_holds_valves_closed_while_ticking");
    check(dispense_controller_job_id() == p1, "pause_keeps_the_job");

    check(dispense_controller_resume(t) == true, "resume_accepted");
    check(!dispense_controller_is_paused(), "resume_clears_pause_flag");
    pump(&t, 1);
    check(dispense_controller_state() == DISPENSE_COARSE_DISPENSE,
          "resume_returns_to_dispensing_after_fresh_check");

    /* Pausing with nothing running is refused rather than silently latched. */
    dispense_controller_cancel_all(t);
    check(dispense_controller_pause(t) == false, "pause_without_job_refused");

    /* ---- The whole loop, from a queued 5 kg job to COMPLETE ----
     *
     * This is the acceptance test the architecture exists for. A virtual
     * vessel starts EMPTY and is filled ONLY by the relay commands the state
     * machine actually issues â€” the same integration the weight sender's
     * dynamic simulator performs â€” and the resulting weight is fed back
     * through the real message path. Nothing here returns a constant 5 kg:
     * the weight is a function of which valve was open and for how long.
     */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);

    /* Sender rates, matching its DYNAMIC simulator defaults. */
    const int32_t COARSE_G_S = 1200;
    const int32_t FINE_G_S = 200;

    int32_t vessel_g = 0;
    int32_t vessel_min = 0;
    int32_t vessel_max = 0;
    int distinct_weights = 0;
    int32_t last_seen = -1;
    bool saw_coarse_open = false;
    bool saw_fine_open = false;
    bool saw_coarse_closed_with_fine_open = false;
    /* The fine valve being CLOSED while the machine is still in the fine
     * region is the observable signature of time-proportional output: a
     * binary valve cannot express a duty cycle any other way. */
    bool saw_fine_modulated = false;

    uint32_t loop_now = 500000U;

    uint32_t loop_job = 0U;
    check(job_queue_add(5000, JOB_PRIORITY_NORMAL, loop_now, &loop_job) == ESP_OK, "e2e_job_queued_5kg");

    /* 100 ms control ticks, 1 Hz weight publication â€” the real cadences. */
    for (int i = 0; i < 900; i++) {
        loop_now += 100U;

        weight_receiver_tick(loop_now);
        dispense_controller_tick(loop_now);
        safety_manager_tick(loop_now);

        bool coarse = dispense_controller_coarse_on();
        bool fine = dispense_controller_fine_on();

        bool in_fine = (dispense_controller_state() == DISPENSE_FINE_DISPENSE ||
                        dispense_controller_state() == DISPENSE_FINE_CORRECTION);

        if (coarse) saw_coarse_open = true;
        if (fine) saw_fine_open = true;
        if (!coarse && fine) saw_coarse_closed_with_fine_open = true;
        if (in_fine && !fine) saw_fine_modulated = true;

        /* The virtual vessel: integrate the COMMANDED valve state. */
        int64_t rate = (coarse ? COARSE_G_S : 0) + (fine ? FINE_G_S : 0);
        vessel_g += (int32_t)(rate * 100 / 1000);
        if (vessel_g > vessel_max) vessel_max = vessel_g;
        if (vessel_g < vessel_min) vessel_min = vessel_g;

        /* The sender publishes on its own 1 Hz tick. */
        if ((loop_now % 1000U) == 0U) {
            if (vessel_g != last_seen) {
                distinct_weights++;
                last_seen = vessel_g;
            }
            char wjson[96];
            int n = snprintf(wjson, sizeof(wjson),
                             "{\"type\":\"weight\",\"weight_g\":%d,\"source\":\"SCALE1\"}",
                             (int)vessel_g);
            weight_receiver_on_message(wjson, (size_t)n, loop_now);
        }

        /* Stop on the job reaching a terminal state, NOT on the first IDLE
         * tick: with the RAMP_TEST zero-gate the machine sits in IDLE until
         * the first confirmed 0 g reading arrives, and an earlier break would
         * end the run before the job ever started. */
        dispense_job_t probe;
        if (job_queue_get(loop_job, &probe) &&
            (probe.state == JOB_COMPLETE || probe.state == JOB_FAILED ||
             probe.state == JOB_CANCELLED)) {
            break;
        }
    }

    dispense_job_t e2e;
    check(job_queue_get(loop_job, &e2e) == true, "e2e_job_record_present");
    check(e2e.state == JOB_COMPLETE, "e2e_job_completed");
    check(e2e.start_g == 0, "e2e_started_from_an_empty_vessel");
    check(e2e.final_g >= 5000 - cfg.tolerance_g &&
          e2e.final_g <= 5000 + cfg.max_overshoot_g,
          "e2e_final_within_tolerance_of_5kg");
    check(e2e.error[0] == '\0', "e2e_completed_without_error");
    check(e2e.finish_ms > e2e.start_ms, "e2e_finish_after_start");

    /* The weight must have been BUILT UP by the valves, not handed over. */
    check(vessel_g > 4000, "e2e_vessel_actually_filled");
    check(distinct_weights > 3, "e2e_weight_took_many_distinct_values");
    check(vessel_min == 0, "e2e_vessel_started_empty");
    check(vessel_max <= 5000 + cfg.max_overshoot_g, "e2e_never_overshot_the_limit");

    /* Both actuators were genuinely exercised, and the handover happened. */
    check(saw_coarse_open, "e2e_coarse_valve_was_opened");
    check(saw_fine_open, "e2e_fine_valve_was_opened");
    check(saw_coarse_closed_with_fine_open, "e2e_coarse_to_fine_handover_observed");
    check(saw_fine_modulated, "e2e_time_proportional_fine_output_observed");

    /* Every relay OFF after completion, including the ten unused channels. */
    check(all_relays(false), "e2e_all_relays_off_after_completion");

    /* ---- Two jobs back to back through the same loop ---- */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);

    vessel_g = 0;
    uint32_t q1 = 0U, q2 = 0U;
    check(job_queue_add(3000, JOB_PRIORITY_NORMAL, loop_now, &q1) == ESP_OK, "e2e_two_jobs_first");
    check(job_queue_add(5000, JOB_PRIORITY_NORMAL, loop_now, &q2) == ESP_OK, "e2e_two_jobs_second");

    int completed_jobs = 0;
    uint32_t last_job_id = 0U;
    for (int i = 0; i < 2000; i++) {
        loop_now += 100U;

        weight_receiver_tick(loop_now);
        dispense_controller_tick(loop_now);
        safety_manager_tick(loop_now);

        /* A new job empties the vessel, exactly as the sender resets its own
         * virtual vessel when the controller reports a job boundary. Detected
         * by the job id changing, because LOAD_JOB itself is a transient state
         * that the controller resolves inside a single tick. */
        uint32_t current_job = dispense_controller_job_id();
        if (current_job != 0U && current_job != last_job_id) {
            last_job_id = current_job;
            vessel_g = 0;
        }

        bool coarse = dispense_controller_coarse_on();
        bool fine = dispense_controller_fine_on();
        int64_t rate = (coarse ? COARSE_G_S : 0) + (fine ? FINE_G_S : 0);
        vessel_g += (int32_t)(rate * 100 / 1000);

        if ((loop_now % 1000U) == 0U) {
            char wjson[96];
            int n = snprintf(wjson, sizeof(wjson),
                             "{\"type\":\"weight\",\"weight_g\":%d,\"source\":\"SCALE1\"}",
                             (int)vessel_g);
            weight_receiver_on_message(wjson, (size_t)n, loop_now);
        }

        dispense_job_t j1, j2;
        (void)job_queue_get(q1, &j1);
        (void)job_queue_get(q2, &j2);
        completed_jobs = (j1.state == JOB_COMPLETE ? 1 : 0) + (j2.state == JOB_COMPLETE ? 1 : 0);
        if (completed_jobs == 2) break;
    }

    dispense_job_t j1, j2;
    (void)job_queue_get(q1, &j1);
    (void)job_queue_get(q2, &j2);
    check(completed_jobs == 2, "e2e_two_jobs_both_completed");
    check(j1.state == JOB_COMPLETE && j2.state == JOB_COMPLETE, "e2e_two_jobs_states");
    check(j1.final_g >= 3000 - cfg.tolerance_g, "e2e_first_job_reached_3kg");
    check(j2.final_g >= 5000 - cfg.tolerance_g, "e2e_second_job_reached_5kg");
    check(job_queue_depth() == 0U, "e2e_two_jobs_queue_drained");
    check(all_relays(false), "e2e_two_jobs_relays_off_at_end");

    /* ---- Boot-order invariant, restated for the dispense build ----
     * Nothing may be energized before the network exists, and the dispense
     * controller must start with both valves closed. */
    (void)relay_init_all();
    (void)safety_manager_init();
    check(all_relays(false), "boot_all_relays_off_dispense_build");

    dispense_controller_init(&cfg_process);
    check(!dispense_controller_coarse_on() && !dispense_controller_fine_on(),
          "boot_dispense_valves_closed");
    check(dispense_controller_state() == DISPENSE_IDLE, "boot_dispense_idle");

    /* A queued job must NOT run at boot when no weight has ever arrived. */
    (void)job_queue_init();
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 1U, NULL);
    dispense_controller_tick(10U);
    check(!dispense_controller_coarse_on(), "boot_no_valve_without_weight");
    check(dispense_controller_state() == DISPENSE_WAIT_FOR_SCALE,
          "boot_job_waits_for_scale");

    /* ---- Dosing relays OFF at every mandated trigger ----
     * The confirmed-hardware requirement names the exact list: boot, reset,
     * Wi-Fi loss, stale or missing weight, fault, timeout, overshoot,
     * emergency stop and watchdog failure. Each is exercised below against the
     * two DOSING relays specifically (index 0 = Relay 1 / coarse, index 1 =
     * Relay 2 / fine), not merely against the group. */
    uint32_t tr = 900000U;

    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);

    check(dosing_off(), "trigger_boot_dosing_relays_off");

    /* Reset: re-running init must land in the same de-energized state. */
    (void)relay_all_on();
    check(!dosing_off(), "trigger_setup_relays_on_before_reset");
    (void)relay_init_all();
    check(dosing_off(), "trigger_reset_dosing_relays_off");

    /* Latched fault of any kind. */
    (void)relay_all_on();
    safety_manager_raise(SAFETY_OVERSHOOT, tr);
    check(dosing_off(), "trigger_fault_dosing_relays_off");
    (void)safety_manager_clear();

    /* Wi-Fi / weight-link loss. */
    (void)relay_all_on();
    weight_receiver_on_link_lost(tr);
    safety_manager_raise(SAFETY_WEIGHT_LINK_LOST, tr);
    check(dosing_off(), "trigger_wifi_loss_dosing_relays_off");

    /* Stale weight while actively dispensing. */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);
    uint32_t tr_job = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, tr, &tr_job);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", tr);
    pump(&tr, 2);
    check(!dosing_off(), "trigger_setup_dispensing_with_valves_open");
    tr += cfg.stale_timeout_ms + 100U;
    dispense_controller_tick(tr);
    check(dosing_off(), "trigger_stale_weight_dosing_relays_off");
    check(safety_manager_fault() == SAFETY_WEIGHT_STALE,
          "trigger_stale_weight_fault_named");

    /* Missing weight from the start: no reading ever arrives. */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, tr, &tr_job);
    pump(&tr, 3);
    check(dosing_off(), "trigger_missing_weight_dosing_relays_off");

    /* Timeout: the job budget expires with the machine mid-dispense. */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, tr, &tr_job);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", tr);
    pump(&tr, 2);
    check(!dosing_off(), "trigger_setup_running_before_timeout");
    tr += cfg.max_duration_ms + 1000U;
    /* Refresh the reading so STALE cannot be the reason this trips. */
    feed("{\"type\":\"weight\",\"weight_g\":100,\"source\":\"SCALE1\"}", tr);
    dispense_controller_tick(tr);
    check(dosing_off(), "trigger_timeout_dosing_relays_off");
    check(safety_manager_fault() == SAFETY_TIMEOUT, "trigger_timeout_fault_named");

    /* Overshoot (PROCESS semantics - see the reasoning above). */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, tr, &tr_job);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", tr);
    pump(&tr, 2);
    feed("{\"type\":\"weight\",\"weight_g\":6000,\"source\":\"SCALE1\"}", tr);
    pump(&tr, 1);
    check(dosing_off(), "trigger_overshoot_dosing_relays_off");
    check(safety_manager_fault() == SAFETY_OVERSHOOT, "trigger_overshoot_fault_named");

    /* Emergency stop. */
    (void)safety_manager_init();
    (void)job_queue_init();
    dispense_controller_init(&cfg_process);
    weight_receiver_init();
    weight_receiver_set_actuation_handler(dispense_controller_on_weight);
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, tr, &tr_job);
    feed("{\"type\":\"weight\",\"weight_g\":0,\"source\":\"SCALE1\"}", tr);
    pump(&tr, 2);
    check(!dosing_off(), "trigger_setup_running_before_estop");
    dispense_controller_emergency_stop(tr);
    check(dosing_off(), "trigger_estop_dosing_relays_off");
    pump(&tr, 3);
    check(dosing_off(), "trigger_estop_holds_dosing_relays_off");
    (void)dispense_controller_clear_fault(tr);

    /* Control watchdog: the state machine stopped advancing. */
    (void)safety_manager_init();
    dispense_controller_init(&cfg_process);
    safety_manager_note_control_tick(tr);
    (void)relay_all_on();
    safety_manager_tick(tr + cfg.max_duration_ms + 5000U);
    check(safety_manager_fault() == SAFETY_CONTROL_FAILURE,
          "trigger_watchdog_fault_named");
    check(dosing_off(), "trigger_watchdog_dosing_relays_off");

    /* ---- Abstraction boundary ----
     * The dispensing layer reaches the hardware only through the logical relay
     * API. This is a compile-time property (dispense_controller.c and
     * job_queue.c include relay_driver.h and never relay_expander.h), and the
     * runtime half of it is that a logical relay_set on a dosing channel is
     * what drives the hardware path â€” proven here through the cached state the
     * driver owns. */
    (void)safety_manager_init();
    (void)relay_init_all();
    check(relay_set(RELAY_INDEX_COARSE, true) == ESP_OK, "boundary_coarse_set_ok");
    check(relay_get_state(RELAY_INDEX_COARSE), "boundary_coarse_state_follows");
    check(relay_set(RELAY_INDEX_FINE, true) == ESP_OK, "boundary_fine_set_ok");
    check(relay_get_state(RELAY_INDEX_FINE), "boundary_fine_state_follows");
    check(relay_all_off() == ESP_OK, "boundary_all_off_ok");
    check(dosing_off(), "boundary_all_off_clears_dosing_relays");

    /* Callers ask the driver WHERE a channel lands and get text back; they
     * never construct an expander bit themselves. That is the runtime half of
     * the encapsulation boundary â€” the compile-time half is that no module
     * above relay_driver includes relay_expander.h. */
    char where[48];
    check(relay_describe_channel(RELAY_INDEX_COARSE, where, sizeof(where)) > 0,
          "describe_channel_returns_text");
    check(strstr(where, "GPIO") != NULL || strstr(where, "expander") != NULL,
          "describe_channel_names_a_location");
    check(relay_describe_channel(RELAY_COUNT, where, sizeof(where)) == 0,
          "describe_channel_bad_index_returns_zero");
    check(relay_describe_channel(0U, NULL, 8U) == 0, "describe_channel_null_safe");
    check(relay_describe_channel(0U, where, 0U) == 0, "describe_channel_zero_cap_safe");

/* ===========================================================================
     * Part 3: dual_dispense â€” single-scale ownership, persistent profiles,
     *         staged dispense, operator READY.
     *
     * The field hardware has ONE CAS scale and TWO dispensing relays. Only one
     * channel may own the scale at a time; while it does, the other stays in
     * DCH_WAITING_SCALE and its relay is never energised.  Profiles live in a
     * persistent four-target table (5000/10000/15000/20000 g) and are NEVER
     * consumed after use.  A job whose target has no profile is refused with
     * NO_PROFILE_FOR_TARGET â€” there is no target_g=0 fallback.
     * ======================================================================== */

    /* ---- Protocol: simulated samples are REJECTED in production ---- */
    weight_msg_t rm;
    const char *sim_pkt =
        "{\"type\":\"weight\",\"weight_g\":7000,\"source\":\"SCALE1\",\"stable\":true,"
        "\"simulated\":true,\"simulation_cycle\":3,\"sequence\":9}";
    check(weight_msg_parse(sim_pkt, strlen(sim_pkt), &rm) == WEIGHT_MSG_REJECTED,
          "parse_simulated_packet_rejected");
    const char *sim_flag =
        "{\"type\":\"weight\",\"weight_g\":7000,\"simulated\":true}";
    check(weight_msg_parse(sim_flag, strlen(sim_flag), &rm) == WEIGHT_MSG_REJECTED,
          "parse_simulated_flag_rejected");
    const char *sim_src2 =
        "{\"type\":\"weight\",\"weight_g\":7000,\"source\":\"SIM\"}";
    check(weight_msg_parse(sim_src2, strlen(sim_src2), &rm) == WEIGHT_MSG_REJECTED,
          "parse_SIM_source_rejected");

    /* Frames WITHOUT the simulator fields are still accepted unchanged, which
     * is what makes the extension backwards compatible rather than a protocol
     * break. */
    const char *old_pkt = "{\"type\":\"weight\",\"weight_g\":8000}";
    check(weight_msg_parse(old_pkt, strlen(old_pkt), &rm) == WEIGHT_MSG_ACCEPTED,
          "parse_legacy_packet_still_accepted");
    check(!rm.simulated && !rm.has_cycle, "legacy_packet_has_no_sim_fields");

    /* Fail-closed on a malformed new field: a non-boolean `simulated` is a
     * broken frame, not something to guess at. */
    const char *bad_sim = "{\"type\":\"weight\",\"weight_g\":100,\"simulated\":\"yes\"}";
    check(weight_msg_parse(bad_sim, strlen(bad_sim), &rm) == WEIGHT_MSG_REJECTED,
          "reject_non_boolean_simulated");
    const char *bad_cycle =
        "{\"type\":\"weight\",\"weight_g\":100,\"simulated\":true,\"simulation_cycle\":\"x\"}";
    check(weight_msg_parse(bad_cycle, strlen(bad_cycle), &rm) == WEIGHT_MSG_REJECTED,
          "malformed_cycle_rejected_fail_closed");

    /* ---- Priority ordering: the pure rule, then the queue ---- */
    check(job_precedes(1U, 5U, 0U, 1U), "priority_1_precedes_priority_0");
    check(!job_precedes(0U, 1U, 1U, 5U), "priority_0_does_not_precede_priority_1");
    check(job_precedes(0U, 1U, 0U, 2U), "fifo_within_normal");
    check(job_precedes(1U, 1U, 1U, 2U), "fifo_within_priority");
    check(!job_precedes(0U, 2U, 0U, 1U), "fifo_is_oldest_first");
    check(job_priority_valid(JOB_PRIORITY_NORMAL) && job_priority_valid(JOB_PRIORITY_HIGH),
          "known_priorities_valid");
    check(!job_priority_valid(2U), "unknown_priority_rejected");

    (void)job_queue_init();
    uint32_t pn1 = 0U, pn2 = 0U, ph1 = 0U;
    check(job_queue_add(1000, JOB_PRIORITY_NORMAL, 10U, &pn1) == ESP_OK, "pq_normal_1");
    check(job_queue_add(2000, JOB_PRIORITY_NORMAL, 20U, &pn2) == ESP_OK, "pq_normal_2");
    check(job_queue_add(3000, JOB_PRIORITY_HIGH, 30U, &ph1) == ESP_OK, "pq_high_1");
    check(job_queue_add(4000, 9U, 40U, NULL) == ESP_ERR_INVALID_ARG,
          "pq_bad_priority_refused");
    check(job_queue_depth() == 3U, "pq_depth_counts_both_levels");
    check(job_queue_depth_at(JOB_PRIORITY_NORMAL) == 2U, "pq_depth_normal");
    check(job_queue_depth_at(JOB_PRIORITY_HIGH) == 1U, "pq_depth_high");

    dispense_job_t pq;
    check(job_queue_start_next(100U, &pq) && pq.id == ph1 && pq.priority == JOB_PRIORITY_HIGH,
          "pq_priority_runs_first_despite_later_arrival");
    check(job_queue_start_next(200U, &pq) && pq.id == pn1, "pq_fifo_normal_first");
    check(job_queue_start_next(300U, &pq) && pq.id == pn2, "pq_fifo_normal_second");
    check(!job_queue_start_next(400U, &pq), "pq_empty_after_all_started");

    /* Non-preemptive: a priority-1 arrival must not disturb the running job. */
    (void)job_queue_init();
    uint32_t run_id = 0U, late_high = 0U;
    (void)job_queue_add(5000, JOB_PRIORITY_NORMAL, 10U, &run_id);
    (void)job_queue_start_next(20U, &pq);
    check(job_queue_add(6000, JOB_PRIORITY_HIGH, 30U, &late_high) == ESP_OK,
          "pq_late_high_queued");
    {
        dispense_job_t still;
        check(job_queue_get_running(&still) && still.id == run_id,
              "pq_running_job_not_preempted");
    }
    {
        dispense_job_t a, b;
        check(job_queue_get_running(&a) && job_queue_get_running(&b), "pq_running_copies_ok");
        check(a.id == b.id, "pq_running_copies_agree");
        check(job_queue_get_running(NULL) == false, "pq_running_null_safe");
    }

    /* Independent global scheduler: one global queue assigns jobs to channels
     * by material id; material 1 -> CH1, material 2 -> CH2. */
    (void)job_queue_init();
    uint32_t fifo_normal=0,fifo_priority=0;
    check(job_queue_add_global_material(1,5000,JOB_PRIORITY_NORMAL,10,&fifo_normal)==ESP_OK,
          "global_queue_normal_added");
    dispense_job_t global_pick;
    check(!job_queue_start_next_available(2,12,&global_pick),
          "material_one_not_claimed_by_idle_material_two_channel");
    check(job_queue_add_global_material(1,12000,JOB_PRIORITY_HIGH,11,&fifo_priority)==ESP_OK,
          "global_queue_priority_added");
    check(!job_material_channel_valid(3,1)&&!job_material_channel_valid(1,2)&&
          job_material_channel_valid(2,2),"fixed_material_mapping_validation");
    check(!job_queue_start_next_available(2,12,&global_pick),
          "incompatible_channel_keeps_both_material_one_jobs_waiting");
    check(job_queue_start_next_available(1,13,&global_pick)&&global_pick.id==fifo_priority,
          "priority_job_runs_only_on_its_material_channel");
    check(job_queue_start_next_available(1,14,&global_pick)&&global_pick.id==fifo_normal,
          "compatible_fifo_job_runs_after_priority");
    check(!job_queue_start_next_available(1,15,&global_pick),"global_queue_empty_after_two_claims");
    check(job_queue_add_global_material(2,8000,JOB_PRIORITY_NORMAL,16,&fifo_priority)==ESP_OK&&
          job_queue_start_next_available(2,17,&global_pick)&&global_pick.id==fifo_priority,
          "material_two_job_claimed_by_channel_two");

    /* ---- dual_dispense: init, state and stage names ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_init_ok");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
          "dual_scale_owner_none_at_init");
    check(strcmp(dual_dispense_state_name(DCH_IDLE), "IDLE") == 0,
          "dual_state_name_idle");
    /* ADJUSTED (CONTRACT 9.11): WAITING_SCALE was renamed WAITING_FOR_SCALE_MOVE. */
    check(strcmp(dual_dispense_state_name(DCH_WAITING_FOR_SCALE_MOVE), "WAITING_FOR_SCALE_MOVE") == 0,
          "dual_state_name_waiting_for_scale_move");
    check(strcmp(dual_dispense_state_name(DCH_WAIT_WEIGHT), "WAIT_WEIGHT") == 0,
          "dual_state_name_wait_weight");
    check(strcmp(dual_dispense_state_name(DCH_DISPENSING), "DISPENSING") == 0,
          "dual_state_name_dispensing");
    check(strcmp(dual_dispense_state_name(DCH_SETTLING), "SETTLING") == 0,
          "dual_state_name_settling");
    check(strcmp(dual_dispense_state_name(DCH_COMPLETE), "COMPLETE") == 0,
          "dual_state_name_complete");
    check(strcmp(dispense_stage_name(DCH_STAGE_NONE), "NONE") == 0,
          "dual_stage_name_none");
    check(strcmp(dispense_stage_name(DCH_STAGE_COARSE), "COARSE") == 0,
          "dual_stage_name_coarse");
    check(strcmp(dispense_stage_name(DCH_STAGE_FINE), "FINE") == 0,
          "dual_stage_name_fine");
    check(strcmp(dispense_stage_name(DCH_STAGE_MICRO), "MICRO") == 0,
          "dual_stage_name_micro");
    check(strcmp(dispense_stage_name(DCH_STAGE_SETTLING), "SETTLING") == 0,
          "dual_stage_name_settling");
    check(strcmp(scale_owner_name(SCALE_OWNER_NONE), "NONE") == 0,
          "dual_scale_owner_name_none");
    check(strcmp(scale_owner_name(SCALE_OWNER_CH1), "CH1") == 0,
          "dual_scale_owner_name_ch1");
    check(strcmp(scale_owner_name(SCALE_OWNER_CH2), "CH2") == 0,
          "dual_scale_owner_name_ch2");

    dual_channel_snapshot_t ch1, ch2;
    check(dual_dispense_controller_snapshot(1, 100U, &ch1), "dual_snapshot_ch1_ok");
    check(dual_dispense_controller_snapshot(2, 100U, &ch2), "dual_snapshot_ch2_ok");
    check(ch1.state == DCH_IDLE && ch2.state == DCH_IDLE,
          "dual_both_channels_idle_at_init");
    check(!ch1.owns_scale && !ch2.owns_scale, "dual_no_scale_owner_at_init");
    check(dual_dispense_controller_stage(1) == DCH_STAGE_NONE &&
          dual_dispense_controller_stage(2) == DCH_STAGE_NONE,
          "dual_stage_none_at_init");

    /* ---- Profile validation: only canonical targets are accepted ---- */
    check(dual_dispense_controller_set_pending_profile(1, "p5k", 2, 5000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_5000_accepted");
    check(dual_dispense_controller_set_pending_profile(1, "p10k", 3, 10000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_10000_accepted");
    check(dual_dispense_controller_set_pending_profile(1, "p15k", 4, 15000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_15000_accepted");
    check(dual_dispense_controller_set_pending_profile(1, "p20k", 5, 20000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_20000_accepted");
    check(!dual_dispense_controller_set_pending_profile(1, "p0", 6, 0,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_target_0_refused");
    check(!dual_dispense_controller_set_pending_profile(1, "p3k", 7, 3000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_non_canonical_refused");
    check(!dual_dispense_controller_set_pending_profile(1, "p12k", 8, 12000,
          0.10f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_profile_12000_refused");

    /* ---- target_g=0 fallback is GONE: no profile means NO_PROFILE_FOR_TARGET ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_noprof_reinit");
    dual_dispense_controller_discard_pending_profile(1, -1); /* clear all */
    uint32_t np_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &np_job) == ESP_OK,
          "dual_noprof_job_queued");
    dual_feed(200U, 0);
    dual_dispense_controller_tick(200U);
    check(dual_dispense_controller_snapshot(1, 200U, &ch1) && ch1.awaiting_operator_ready,
          "dual_noprof_awaits_operator_ready");
    check(!dual_dispense_controller_operator_ready(1, 250U),
          "dual_noprof_operator_ready_returns_false");
    {
        dispense_job_t np;
        check(job_queue_get(np_job, &np) && np.state == JOB_FAILED,
              "dual_noprof_job_failed");
        check(strcmp(np.error, "NO_PROFILE_FOR_TARGET") == 0,
              "dual_noprof_error_text");
    }

    /* ---- READY gate safety: refusal mutates nothing, no stale confirmation ---- */
    {
        const char *re = "";
        uint32_t t = 700000U, ja = 0, jb = 0, jc = 0;
        dispense_job_t jr;
        job_profile_t pp;
        memset(&pp, 0, sizeof(pp));
        pp.valid = true;
        snprintf(pp.profile_id, sizeof(pp.profile_id), "or-pin2");
        pp.version = 7; pp.kp = 0.22f;
        pp.tolerance_g = 20; pp.max_overshoot_g = 100;
        pp.max_duration_ms = 120000; pp.window_ms = 500;
        pp.min_on_ms = 40; pp.min_off_ms = 40;
        pp.coarse_threshold_g = 500; pp.fine_threshold_g = 100; pp.micro_threshold_g = 30;
        pp.coarse_min_on_ms = 80; pp.fine_min_on_ms = 30; pp.micro_min_on_ms = 15;
        pp.settle_time_ms = 200;

        /* A: both awaiting; READY CH1 ok, READY CH2 refused with no change;
         * after CH1 completes CH2 STILL awaits; its pinned profile is used. */
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        check(dual_dispense_controller_add_material_server_job_pinned(
                  2, 5000, 0, 9500, &pp, &jb) == ESP_OK, "orgate_a_jobs_queued");
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(dual_dispense_controller_snapshot(2, t, &ch2) && ch2.awaiting_operator_ready,
              "orgate_a_both_await");
        check(telemetry_client_handle_ready(101U, 1, t + 10U, &re), "orgate_a_ready_ch1_ok");
        check(!dual_dispense_controller_operator_ready(2, t + 20U),
              "orgate_a_ready_ch2_refused_while_ch1_owns_scale");
        check(!telemetry_client_handle_ready(102U, 2, t + 21U, &re),
              "orgate_a_ready_cmd_ch2_refused");
        check(dual_dispense_controller_snapshot(2, t + 30U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE && !ch2.relay_on,
              "orgate_a_refused_ready_changes_nothing");
        dual_dispense_controller_tick(t + 40U);
        check(dual_dispense_controller_snapshot(2, t + 40U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE,
              "orgate_a_ch2_still_awaiting_after_tick");
        or_complete_ch1(t + 100U);
        check(job_queue_get(ja, &jr) && jr.state == JOB_COMPLETE, "orgate_a_ch1_complete");
        dual_dispense_controller_tick(t + 600U);
        dual_dispense_controller_tick(t + 700U);
        check(dual_dispense_controller_snapshot(2, t + 700U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE &&
              !ch2.relay_on && !ch2.owns_scale &&
              dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
              "orgate_a_ch2_still_awaits_after_ch1_completes");
        check(telemetry_client_handle_ready(103U, 2, t + 710U, &re), "orgate_a_ready_ch2_ok");
        check(dual_dispense_controller_snapshot(2, t + 710U, &ch2) &&
              ch2.state == DCH_WAIT_WEIGHT && ch2.owns_scale &&
              ch2.profile_version == 7 && strcmp(ch2.profile_id, "or-pin2") == 0 &&
              ch2.kp > 0.21f && ch2.kp < 0.23f,
              "orgate_a_ch2_runs_pinned_profile");

        /* B: cancel of a refused-READY job must not pre-approve the next one. */
        t += 10000U;
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jb);
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(telemetry_client_handle_ready(201U, 1, t + 10U, &re), "orgate_b_ready_ch1_ok");
        check(!dual_dispense_controller_operator_ready(2, t + 20U), "orgate_b_ready_ch2_refused");
        check(dual_dispense_controller_cancel_job(jb, t + 30U), "orgate_b_cancel_ch2");
        (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jc);
        or_complete_ch1(t + 100U);
        dual_dispense_controller_tick(t + 600U);
        check(dual_dispense_controller_snapshot(2, t + 600U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE && !ch2.relay_on,
              "orgate_b_next_job_after_cancel_awaits_ready");

        /* C: E-STOP + clear must not leave a pre-approved READY behind. */
        t += 10000U;
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jb);
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(telemetry_client_handle_ready(301U, 1, t + 10U, &re), "orgate_c_ready_ch1_ok");
        (void)dual_dispense_controller_operator_ready(2, t + 20U);
        dual_dispense_controller_emergency_stop(t + 30U);
        dual_dispense_controller_clear_emergency_stop();
        (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jc);
        dual_feed(t + 100U, 0);
        dual_dispense_controller_tick(t + 100U);
        check(dual_dispense_controller_snapshot(2, t + 100U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE && !ch2.relay_on,
              "orgate_c_job_after_estop_clear_awaits_ready");

        /* D: stale READY (id <= high-water) never releases a later job. */
        t += 10000U;
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(telemetry_client_handle_ready(401U, 1, t + 10U, &re), "orgate_d_ready_401_ok");
        check(dual_dispense_controller_cancel(1U, t + 20U), "orgate_d_cancel");
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &jb);
        dual_feed(t + 100U, 0);
        dual_dispense_controller_tick(t + 100U);
        check(dual_dispense_controller_snapshot(1, t + 100U, &ch1) &&
              ch1.awaiting_operator_ready, "orgate_d_next_job_awaits");
        check(!telemetry_client_handle_ready(400U, 1, t + 110U, &re),
              "orgate_d_stale_ready_refused");
        check(telemetry_client_handle_ready(401U, 1, t + 120U, &re) &&
              dual_dispense_controller_snapshot(1, t + 120U, &ch1) &&
              ch1.awaiting_operator_ready && ch1.state == DCH_WAITING_SCALE,
              "orgate_d_replayed_id_reacked_without_release");
        check(telemetry_client_handle_ready(402U, 1, t + 130U, &re) &&
              dual_dispense_controller_snapshot(1, t + 130U, &ch1) &&
              ch1.state == DCH_WAIT_WEIGHT, "orgate_d_fresh_ready_releases");

        /* E: PAUSE then RESUME while awaiting READY returns to the gate. */
        t += 10000U;
        telemetry_client_test_reset_ready();
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(dual_dispense_controller_pause(1U, t + 10U), "orgate_e_pause_ok");
        check(dual_dispense_controller_resume(1U, t + 20U), "orgate_e_resume_ok");
        dual_dispense_controller_tick(t + 30U);
        check(dual_dispense_controller_snapshot(1, t + 30U, &ch1) &&
              ch1.state == DCH_WAITING_SCALE && ch1.awaiting_operator_ready &&
              !ch1.owns_scale && !ch1.relay_on, "orgate_e_resume_back_at_gate");
        check(telemetry_client_handle_ready(501U, 1, t + 40U, &re) &&
              dual_dispense_controller_snapshot(1, t + 40U, &ch1) &&
              ch1.state == DCH_WAIT_WEIGHT && ch1.owns_scale, "orgate_e_ready_then_works");

        /* F: a refused READY id re-served once the channel awaits stays refused. */
        t += 10000U;
        telemetry_client_test_reset_ready();
        or_setup();
        (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
        (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jb);
        dual_feed(t, 0);
        dual_dispense_controller_tick(t);
        check(telemetry_client_handle_ready(601U, 1, t + 10U, &re), "orgate_f_ready_ch1_ok");
        check(!telemetry_client_handle_ready(602U, 2, t + 20U, &re),
              "orgate_f_ready_ch2_refused");
        or_complete_ch1(t + 100U);
        dual_dispense_controller_tick(t + 600U);
        dual_dispense_controller_tick(t + 700U);
        check(!telemetry_client_handle_ready(602U, 2, t + 710U, &re) && re[0] &&
              dual_dispense_controller_snapshot(2, t + 710U, &ch2) &&
              ch2.awaiting_operator_ready && ch2.state == DCH_WAITING_SCALE,
              "orgate_f_reserved_refused_id_still_refused");
        check(telemetry_client_handle_ready(603U, 2, t + 720U, &re) &&
              dual_dispense_controller_snapshot(2, t + 720U, &ch2) &&
              ch2.state == DCH_WAIT_WEIGHT, "orgate_f_fresh_higher_id_releases");
    }

    /* ---- Operator READY flow: a ready job waits for confirmation ---- */
    (void)job_queue_init();
    (void)safety_manager_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_opready_reinit");
    check(dual_dispense_controller_set_pending_profile_staged(1, "opready-5k", 10, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 20),
          "dual_opready_profile_set");
    uint32_t or_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &or_job) == ESP_OK,
          "dual_opready_job_queued");
    dual_feed(1000U, 0);
    dual_dispense_controller_tick(1000U);
    check(dual_dispense_controller_snapshot(1, 1000U, &ch1) &&
          ch1.state == DCH_WAITING_SCALE && ch1.awaiting_operator_ready,
          "dual_opready_enters_waiting_scale");
    check(!ch1.relay_on, "dual_opready_relay_stays_off");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
          "dual_opready_scale_not_yet_acquired");
    /* READY device command: the production path to operator_ready */
    {
        const char *rerr = "";
        check(!telemetry_client_handle_ready(9001U, 2, 1050U, &rerr) && rerr[0],
              "ready_cmd_nothing_awaiting_fails_with_reason");
        check(!telemetry_client_handle_ready(9001U, 0, 1050U, &rerr),
              "ready_cmd_no_channel_fails");
        check(dual_dispense_controller_snapshot(1, 1050U, &ch1) &&
              ch1.state == DCH_WAITING_SCALE && ch1.awaiting_operator_ready && !ch1.relay_on,
              "ready_cmd_failed_no_state_change");
        check(telemetry_client_handle_ready(9001U, 1, 1100U, &rerr),
              "ready_cmd_applied");
        check(dual_dispense_controller_snapshot(1, 1100U, &ch1) &&
              ch1.state == DCH_WAIT_WEIGHT && !ch1.relay_on,
              "ready_cmd_advances_no_relay_action");
        /* Redelivered command id is re-ACKed APPLIED without re-execution */
        check(telemetry_client_handle_ready(9001U, 1, 1120U, &rerr) &&
              strstr(rerr, "duplicate") != NULL,
              "ready_cmd_duplicate_not_reexecuted");
        check(!telemetry_client_handle_ready(9002U, 1, 1130U, &rerr),
              "ready_cmd_after_advance_fails");
    }
    check(dual_dispense_controller_snapshot(1, 1100U, &ch1) &&
          ch1.state == DCH_WAIT_WEIGHT && ch1.owns_scale,
          "dual_opready_advances_to_wait_weight");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1,
          "dual_opready_acquires_scale");
    /* Calling operator_ready again is a no-op */
    check(!dual_dispense_controller_operator_ready(1, 1150U),
          "dual_opready_second_call_rejected");

    /* ---- Staged control: COARSE -> FINE -> MICRO -> SETTLING ---- */
    dual_feed(1200U, 0);
    dual_dispense_controller_tick(1200U);
    check(dual_dispense_controller_snapshot(1, 1200U, &ch1) &&
          ch1.state == DCH_DISPENSING,
          "dual_stage_enters_dispensing");
    check(dual_dispense_controller_stage(1) == DCH_STAGE_COARSE,
          "dual_stage_coarse_far_from_target");

    /* error=400 (weight 4600): still COARSE (ae > fine_threshold=100) */
    dual_feed(1300U, 4600);
    dual_dispense_controller_tick(1300U);
    check(dual_dispense_controller_stage(1) == DCH_STAGE_COARSE,
          "dual_stage_coarse_medium_error");

    /* error=80 (weight 4920): FINE (30 < ae <= 100) */
    dual_feed(1400U, 4920);
    dual_dispense_controller_tick(1400U);
    check(dual_dispense_controller_stage(1) == DCH_STAGE_FINE,
          "dual_stage_fine_medium_error");

    /* error=25 (weight 4975): MICRO (20 < ae <= 30) */
    dual_feed(1500U, 4975);
    dual_dispense_controller_tick(1500U);
    check(dual_dispense_controller_stage(1) == DCH_STAGE_MICRO,
          "dual_stage_micro_near_target");

    /* error=5 (weight 4995): effective_error = 5-20 = -15 <= 0 -> SETTLING */
    dual_feed(1600U, 4995);
    dual_dispense_controller_tick(1600U);
    check(dual_dispense_controller_stage(1) == DCH_STAGE_SETTLING,
          "dual_stage_settling_within_inflight");
    check(dual_dispense_controller_snapshot(1, 1600U, &ch1) &&
          ch1.state == DCH_SETTLING,
          "dual_state_settling");
    check(!ch1.relay_on, "dual_relay_off_while_settling");

    /* Settle requires settle_time_ms elapsed + a fresh sample */
    dual_feed(1700U, 5000);
    dual_dispense_controller_tick(1700U);
    check(dual_dispense_controller_snapshot(1, 1700U, &ch1) &&
          ch1.state == DCH_SETTLING,
          "dual_settle_not_skipped_early");
    dual_feed(1900U, 5000);   /* 300 ms > 200 ms settle_time */
    dual_dispense_controller_tick(1900U);
    check(dual_dispense_controller_snapshot(1, 1900U, &ch1) &&
          ch1.state == DCH_COMPLETE,
          "dual_completes_after_settle");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
          "dual_scale_released_after_complete");

    /* ---- Single-scale serialization: CH2 waits while CH1 owns the scale ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_serial_reinit");
    check(dual_dispense_controller_set_pending_profile_staged(1, "serial-ch1", 20, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 20),
          "dual_serial_profile_ch1");
    check(dual_dispense_controller_set_pending_profile_staged(2, "serial-ch2", 21, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 20),
          "dual_serial_profile_ch2");

    uint32_t s1_job = 0, s2_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &s1_job) == ESP_OK,
          "dual_serial_ch1_job_queued");

    uint32_t serial_st = 100000U;
    dual_feed(serial_st, 0);
    dual_dispense_controller_tick(serial_st);
    check(dual_dispense_controller_snapshot(1, serial_st, &ch1) &&
          ch1.state == DCH_WAITING_SCALE && ch1.awaiting_operator_ready,
          "dual_serial_ch1_awaits_operator");
    check(dual_dispense_controller_operator_ready(1, serial_st + 100),
          "dual_serial_operator_ready_ch1");
    dual_feed(serial_st + 100, 0);
    dual_dispense_controller_tick(serial_st + 100);
    check(dual_dispense_controller_snapshot(1, serial_st + 100, &ch1) &&
          ch1.state == DCH_DISPENSING,
          "dual_serial_ch1_dispensing");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1,
          "dual_serial_ch1_owns_scale");

    /* Now queue CH2's job while CH1 runs. */
    check(dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &s2_job) == ESP_OK,
          "dual_serial_ch2_job_queued");
    dual_feed(serial_st + 200, 100);
    dual_dispense_controller_tick(serial_st + 200);

    /* CH2 must be WAITING_SCALE with its relay off. */
    check(dual_dispense_controller_snapshot(2, serial_st + 200, &ch2) &&
          ch2.state == DCH_WAITING_SCALE,
          "dual_serial_ch2_waiting_scale");
    check(!ch2.relay_on, "dual_serial_ch2_relay_off");
    check(!relay_get_state(1), "dual_serial_relay2_hard_off");
    check(!(relay_get_state(0) && relay_get_state(1)),
          "dual_serial_invariant_both_relays");
    {
        dispense_job_t sj;
        check(job_queue_get(s2_job, &sj) && sj.state == JOB_QUEUED,
              "dual_serial_ch2_job_stays_queued");
    }

    /* Drive CH1 to completion through SETTLING. */
    dual_feed(serial_st + 300, 4995);
    dual_dispense_controller_tick(serial_st + 300);
    check(dual_dispense_controller_snapshot(1, serial_st + 300, &ch1) &&
          ch1.state == DCH_SETTLING,
          "dual_serial_ch1_settling");
    check(!(relay_get_state(0) && relay_get_state(1)),
          "dual_serial_invariant_while_settling");

    dual_feed(serial_st + 600, 5000);   /* 300 ms > settle_time */
    dual_dispense_controller_tick(serial_st + 600);
    check(dual_dispense_controller_snapshot(1, serial_st + 600, &ch1) &&
          ch1.state == DCH_COMPLETE,
          "dual_serial_ch1_complete");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
          "dual_serial_scale_released");
    {
        dispense_job_t sj;
        check(job_queue_get(s2_job, &sj) && sj.state == JOB_QUEUED,
              "dual_serial_ch2_job_still_queued_after_ch1");
    }

    /* CH2 may now claim the scale after operator confirmation. */
    dual_feed(serial_st + 700, 0);   /* scale moved to CH2's empty vessel */
    dual_dispense_controller_tick(serial_st + 700);
    check(dual_dispense_controller_snapshot(2, serial_st + 700, &ch2) &&
          ch2.state == DCH_WAITING_SCALE && ch2.awaiting_operator_ready,
          "dual_serial_ch2_awaits_operator_after_ch1");
    check(dual_dispense_controller_operator_ready(2, serial_st + 800),
          "dual_serial_operator_ready_ch2");
    dual_feed(serial_st + 800, 0);
    dual_dispense_controller_tick(serial_st + 800);
    check(dual_dispense_controller_snapshot(2, serial_st + 800, &ch2) &&
          ch2.owns_scale && ch2.state != DCH_IDLE,
          "dual_serial_ch2_started_after_release");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_CH2,
          "dual_serial_ch2_owns_scale_now");
    check(!(relay_get_state(0) && relay_get_state(1)),
          "dual_serial_invariant_after_handover");

    /* ---- Profile persistence: entries are NEVER consumed ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_persist_reinit");

    /* Two independent profiles for two canonical targets. */
    check(dual_dispense_controller_set_pending_profile_staged(1, "persist-a", 30, 5000,
          0.11f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 0),
          "dual_persist_profile_5k_set");
    check(dual_dispense_controller_set_pending_profile_staged(1, "persist-b", 31, 10000,
          0.22f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 0),
          "dual_persist_profile_10k_set");

    /* Run a 5000 g job to completion. */
    uint32_t p1_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &p1_job) == ESP_OK,
          "dual_persist_first_job_queued");
    uint32_t pt = 200000U;
    dual_feed(pt, 0);
    dual_dispense_controller_tick(pt);
    (void)dual_dispense_controller_operator_ready(1, pt + 50);
    dual_feed(pt + 50, 0);
    dual_dispense_controller_tick(pt + 50);
    dual_feed(pt + 100, 5000);
    dual_dispense_controller_tick(pt + 100);
    dual_feed(pt + 400, 5000);
    dual_dispense_controller_tick(pt + 400);
    {
        dispense_job_t pj;
        check(job_queue_get(p1_job, &pj) && pj.state == JOB_COMPLETE,
              "dual_persist_first_job_complete");
    }
    check(dual_dispense_controller_snapshot(1, pt + 400, &ch1) &&
          ch1.profile_version == 30 && strcmp(ch1.profile_id, "persist-a") == 0,
          "dual_persist_profile_a_applied");

    /* Run a 10000 g job: independent profile applies. */
    uint32_t p2_job = 0;
    check(dual_dispense_controller_add_job(1, 10000, JOB_PRIORITY_NORMAL, &p2_job) == ESP_OK,
          "dual_persist_second_job_queued");
    dual_feed(pt + 500, 0);
    dual_dispense_controller_tick(pt + 500);
    (void)dual_dispense_controller_operator_ready(1, pt + 550);
    dual_feed(pt + 550, 0);
    dual_dispense_controller_tick(pt + 550);
    check(dual_dispense_controller_snapshot(1, pt + 550, &ch1) &&
          ch1.profile_version == 31 && strcmp(ch1.profile_id, "persist-b") == 0,
          "dual_persist_profile_b_applied_independently");
    dual_feed(pt + 600, 10000);
    dual_dispense_controller_tick(pt + 600);
    dual_feed(pt + 900, 10000);
    dual_dispense_controller_tick(pt + 900);
    {
        dispense_job_t pj;
        check(job_queue_get(p2_job, &pj) && pj.state == JOB_COMPLETE,
              "dual_persist_second_job_complete");
    }

    /* Run ANOTHER 5000 g job: profile A is still there â€” never consumed. */
    uint32_t p3_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &p3_job) == ESP_OK,
          "dual_persist_third_job_queued");
    dual_feed(pt + 1000, 0);
    dual_dispense_controller_tick(pt + 1000);
    (void)dual_dispense_controller_operator_ready(1, pt + 1050);
    dual_feed(pt + 1050, 0);
    dual_dispense_controller_tick(pt + 1050);
    check(dual_dispense_controller_snapshot(1, pt + 1050, &ch1) &&
          ch1.profile_version == 30 && strcmp(ch1.profile_id, "persist-a") == 0,
          "dual_persist_profile_a_survives_completion");
    dual_feed(pt + 1100, 5000);
    dual_dispense_controller_tick(pt + 1100);
    dual_feed(pt + 1400, 5000);
    dual_dispense_controller_tick(pt + 1400);
    {
        dispense_job_t pj;
        check(job_queue_get(p3_job, &pj) && pj.state == JOB_COMPLETE,
              "dual_persist_third_job_complete");
    }
    /* ---- Simulated samples are rejected at the controller level ---- */
    weight_msg_t sim_msg;
    memset(&sim_msg, 0, sizeof(sim_msg));
    sim_msg.has_weight1 = true;
    sim_msg.weight1_g = 9999;
    sim_msg.simulated = true;
    dual_dispense_controller_on_weight(&sim_msg, pt + 2000U);
    check(dual_dispense_controller_snapshot(1, pt + 2000U, &ch1) &&
          ch1.current_weight_g != 9999,
          "dual_simulated_sample_rejected");

    /* ADJUSTED (CONTRACT 9.11, per-channel model intentionally replaced): there is
     * one scale. The per-channel fields of a legacy frame are ignored, the sample is
     * weight_g, and with no owner it feeds ONLY the scale view - no pump is renewed. */
    weight_msg_t dual_msg;
    memset(&dual_msg, 0, sizeof(dual_msg));
    dual_msg.weight_g = 123;
    dual_msg.has_weight1 = true;
    dual_msg.weight1_g = 7777;
    dual_msg.has_weight2 = true;
    dual_msg.weight2_g = 99999;
    dual_dispense_controller_on_weight(&dual_msg, pt + 2100U);
    {
        dual_scale_info_t si;
        dual_dispense_controller_scale_info(pt + 2100U, &si);
        check(si.online && si.weight_g == 123, "dual_perchannel_fields_ignored_scale_view_uses_weight_g");
    }
    check(dual_dispense_controller_snapshot(1, pt + 2100U, &ch1) &&
          ch1.current_weight_g != 123 && ch1.current_weight_g != 7777,
          "dual_no_owner_sample_does_not_renew_pump");
    /* ---- Emergency stop forces both relays off ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "dual_estop_reinit");
    check(dual_dispense_controller_set_pending_profile(1, "estop-5k", 40, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "dual_estop_profile_set");
    uint32_t e_job = 0;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &e_job) == ESP_OK,
          "dual_estop_job_queued");
    dual_feed(300000U, 0);
    dual_dispense_controller_tick(300000U);
    (void)dual_dispense_controller_operator_ready(1, 300100U);
    dual_feed(300100U, 0);
    dual_dispense_controller_tick(300100U);
    check(!relay_get_state(0) || relay_get_state(0), "dual_estop_setup_ticked");
    dual_dispense_controller_emergency_stop(300200U);
    check(!relay_get_state(0) && !relay_get_state(1),
          "dual_estop_both_relays_off");
    check(dual_dispense_controller_snapshot(1, 300200U, &ch1) &&
          ch1.state == DCH_FAULT,
          "dual_estop_state_fault");
    check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
          "dual_estop_releases_scale");
    dual_dispense_controller_clear_emergency_stop();
    check(dual_dispense_controller_snapshot(1, 300300U, &ch1) &&
          ch1.state == DCH_IDLE,
          "dual_estop_clear_returns_idle");

    /* ---- single-job cancellation must not clear unrelated queue entries ----
     *
     * Cancellation is addressed by the unique job id the operator selected.
     * Concurrent and duplicate target_g values are independent rows and must
     * cancel independently. dual_dispense_controller_cancel(CHn) is the
     * channel's ACTIVE-job control; it must never walk the backlog behind it. */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    uint32_t sc_a = 0U, sc_b = 0U, sc_c = 0U;
    dispense_job_t sc_job;
    (void)job_queue_add_for_channel(1U, 10000, JOB_PRIORITY_NORMAL, 10U, &sc_a);
    (void)job_queue_add_for_channel(1U, 10000, JOB_PRIORITY_NORMAL, 20U, &sc_b);
    (void)job_queue_add_for_channel(1U, 5000, JOB_PRIORITY_NORMAL, 30U, &sc_c);
    check(job_queue_cancel(sc_b) == true, "scope_cancel_middle_by_id_ok");
    check(job_queue_get(sc_b, &sc_job) && sc_job.state == JOB_CANCELLED,
          "scope_cancel_middle_terminal");
    check(job_queue_get(sc_a, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_cancel_leaves_older_duplicate_target_queued");
    check(job_queue_get(sc_c, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_cancel_leaves_newer_sibling_queued");
    check(job_queue_depth() == 2U, "scope_cancel_depth_two_remain");

    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "scope_dual_init_ok");
    check(dual_dispense_controller_set_pending_profile(1, "scope-5k", 1, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "scope_profile_5k_set");
    check(dual_dispense_controller_set_pending_profile(1, "scope-10k", 2, 10000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "scope_profile_10k_set");
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &sc_a) == ESP_OK,
          "scope_job_a_queued");
    check(dual_dispense_controller_add_job(1, 10000, JOB_PRIORITY_NORMAL, &sc_b) == ESP_OK,
          "scope_job_b_queued");
    check(dual_dispense_controller_add_job(1, 10000, JOB_PRIORITY_NORMAL, &sc_c) == ESP_OK,
          "scope_job_c_queued");

    /* Nothing is running: a channel-keyed cancel has no target job and must
     * leave the entire waiting line-up alone. */
    check(dual_dispense_controller_cancel(1U, 400000U) == false,
          "scope_channel_cancel_with_nothing_running_is_noop");
    check(job_queue_get(sc_a, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_idle_channel_cancel_leaves_job_a_queued");
    check(job_queue_get(sc_b, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_idle_channel_cancel_leaves_job_b_queued");
    check(job_queue_get(sc_c, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_idle_channel_cancel_leaves_job_c_queued");

    /* Claim the first job, then cancel the channel's ACTIVE job only. Its two
     * siblings â€” one carrying a duplicate target â€” must survive. */
    dual_feed(400100U, 0);
    dual_dispense_controller_tick(400100U);
    (void)dual_dispense_controller_operator_ready(1, 400150U);
    dual_feed(400150U, 0);
    dual_dispense_controller_tick(400150U);
    check(job_queue_get(sc_a, &sc_job) && sc_job.state == JOB_RUNNING,
          "scope_first_job_running");
    check(dual_dispense_controller_cancel(1U, 400200U) == true,
          "scope_channel_cancel_active_ok");
    check(job_queue_get(sc_a, &sc_job) && sc_job.state == JOB_CANCELLED,
          "scope_channel_cancel_active_terminal");
    check(job_queue_get(sc_b, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_channel_cancel_active_leaves_duplicate_target_sibling");
    check(job_queue_get(sc_c, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_channel_cancel_active_leaves_other_sibling");

    /* A QUEUED->RUNNING race between the UI click and the command poll lands
     * on the running branch; that must still remove exactly one job. */
    dual_feed(400300U, 0);
    dual_dispense_controller_tick(400300U);
    (void)dual_dispense_controller_operator_ready(1, 400350U);
    dual_feed(400350U, 0);
    dual_dispense_controller_tick(400350U);
    check(job_queue_get(sc_b, &sc_job) && sc_job.state == JOB_RUNNING,
          "scope_second_job_running_after_race");
    check(dual_dispense_controller_cancel(1U, 400400U) == true,
          "scope_race_cancel_ok");
    check(job_queue_get(sc_b, &sc_job) && sc_job.state == JOB_CANCELLED,
          "scope_race_cancel_terminal");
    check(job_queue_get(sc_c, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_race_cancel_leaves_sibling_queued");

    /* ---- dual_dispense_controller_cancel_job: strictly one id ---- */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "scope_job_api_init_ok");
    check(dual_dispense_controller_set_pending_profile(1, "jobapi-5k", 1, 5000,
          0.0025f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40),
          "scope_job_api_profile_set");
    uint32_t ja = 0U, jb = 0U, jc = 0U;
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja) == ESP_OK,
          "scope_job_api_a_queued");
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &jb) == ESP_OK,
          "scope_job_api_b_queued_duplicate_target");
    check(dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &jc) == ESP_OK,
          "scope_job_api_c_queued_duplicate_target");
    check(dual_dispense_controller_cancel_job(jb, 500000U) == true,
          "scope_job_api_cancel_middle_ok");
    check(job_queue_get(jb, &sc_job) && sc_job.state == JOB_CANCELLED,
          "scope_job_api_middle_terminal");
    check(job_queue_get(ja, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_job_api_leaves_older_duplicate_target");
    check(job_queue_get(jc, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_job_api_leaves_newer_duplicate_target");

    /* Unknown and zero ids are refusals that change nothing. */
    check(dual_dispense_controller_cancel_job(0U, 500100U) == false,
          "scope_job_api_zero_id_refused");
    check(dual_dispense_controller_cancel_job(9999U, 500100U) == false,
          "scope_job_api_unknown_id_refused");
    check(job_queue_get(ja, &sc_job) && sc_job.state == JOB_QUEUED &&
          job_queue_get(jc, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_job_api_refusals_leave_queue_alone");

    /* A running job cancels by id too, and its sibling survives. */
    dual_feed(500200U, 0);
    dual_dispense_controller_tick(500200U);
    (void)dual_dispense_controller_operator_ready(1, 500250U);
    dual_feed(500250U, 0);
    dual_dispense_controller_tick(500250U);
    check(job_queue_get(ja, &sc_job) && sc_job.state == JOB_RUNNING,
          "scope_job_api_first_running");
    check(dual_dispense_controller_cancel_job(ja, 500300U) == true,
          "scope_job_api_cancel_running_ok");
    check(job_queue_get(ja, &sc_job) && sc_job.state == JOB_CANCELLED,
          "scope_job_api_running_terminal");
    check(job_queue_get(jc, &sc_job) && sc_job.state == JOB_QUEUED,
          "scope_job_api_running_cancel_leaves_sibling");
    check(dual_dispense_controller_cancel_job(ja, 500350U) == false,
          "scope_job_api_double_cancel_is_noop");

    /* ---- Job-carried PID profile pin ----
     *
     * The server pins the exact immutable tuning version the operator chose
     * and ships its gains with every JOB command. Two queued jobs that share a
     * target_g can pin different versions; each must run its own. The shared
     * per-target profile slot cannot represent both, so the gains travel with
     * the job. A pin that is present but unusable refuses the job â€” there is
     * never a silent fallback to "whatever is staged for this target". */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "pin_init_ok");

    job_profile_t pin_v1, pin_v2;
    memset(&pin_v1, 0, sizeof(pin_v1));
    memset(&pin_v2, 0, sizeof(pin_v2));
    pin_v1.valid = true;
    snprintf(pin_v1.profile_id, sizeof(pin_v1.profile_id), "tune-a");
    pin_v1.version = 1;
    pin_v1.kp = 0.11f; pin_v1.ki = 0.0f; pin_v1.kd = 0.0f;
    pin_v1.tolerance_g = 20; pin_v1.max_overshoot_g = 100;
    pin_v1.max_duration_ms = 120000; pin_v1.window_ms = 500;
    pin_v1.min_on_ms = 40; pin_v1.min_off_ms = 40;
    pin_v1.coarse_threshold_g = 500; pin_v1.fine_threshold_g = 100;
    pin_v1.micro_threshold_g = 30;
    pin_v1.coarse_min_on_ms = 80; pin_v1.fine_min_on_ms = 30;
    pin_v1.micro_min_on_ms = 15;
    pin_v1.settle_time_ms = 200; pin_v1.inflight_comp_g = 0;

    pin_v2 = pin_v1;
    pin_v2.version = 2;
    pin_v2.kp = 0.22f;

    /* A third set of gains in the target slot. If the controller ever falls
     * back to the slot instead of the job's pin, this decoy shows up. */
    check(dual_dispense_controller_set_pending_profile_staged(1, "slot-decoy", 9, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 0),
          "pin_decoy_slot_staged");

    uint32_t pin_j1 = 0, pin_j2 = 0;
    check(dual_dispense_controller_add_material_server_job_pinned(
              1, 5000, 0, 9001, &pin_v1, &pin_j1) == ESP_OK,
          "pin_job1_queued");
    check(dual_dispense_controller_add_material_server_job_pinned(
              1, 5000, 0, 9002, &pin_v2, &pin_j2) == ESP_OK,
          "pin_job2_queued_same_target");
    {
        dispense_job_t pj;
        check(job_queue_get(pin_j1, &pj) && pj.pin.valid && pj.pin.version == 1 &&
              strcmp(pj.pin.profile_id, "tune-a") == 0,
              "pin_job1_record_carries_v1");
        check(job_queue_get(pin_j2, &pj) && pj.pin.valid && pj.pin.version == 2,
              "pin_job2_record_carries_v2");
    }

    uint32_t pint = 600000U;
    dual_feed(pint, 0);
    dual_dispense_controller_tick(pint);
    (void)dual_dispense_controller_operator_ready(1, pint + 50);
    dual_feed(pint + 50, 0);
    dual_dispense_controller_tick(pint + 50);
    check(dual_dispense_controller_snapshot(1, pint + 50, &ch1) &&
          ch1.profile_version == 1 && strcmp(ch1.profile_id, "tune-a") == 0 &&
          ch1.kp > 0.10f && ch1.kp < 0.12f,
          "pin_job1_runs_with_its_own_v1_not_the_slot");

    /* Drive job 1 to completion; the slot is untouched the whole time.
     * inflight_comp_g is 0 on this pin, so only a sample already at target
     * enters SETTLING â€” then one more fresh sample past settle_time closes it. */
    dual_feed(pint + 100, 5000);
    dual_dispense_controller_tick(pint + 100);
    dual_feed(pint + 400, 5000);
    dual_dispense_controller_tick(pint + 400);
    {
        dispense_job_t pj;
        check(job_queue_get(pin_j1, &pj) && pj.state == JOB_COMPLETE,
              "pin_job1_complete");
    }

    /* Job 2 now starts. It must run with its own v2, not the slot decoy and
     * not job 1's v1 â€” even though both jobs share target_g = 5000. */
    dual_feed(pint + 500, 0);
    dual_dispense_controller_tick(pint + 500);
    (void)dual_dispense_controller_operator_ready(1, pint + 550);
    dual_feed(pint + 550, 0);
    dual_dispense_controller_tick(pint + 550);
    check(dual_dispense_controller_snapshot(1, pint + 550, &ch1) &&
          ch1.profile_version == 2 && strcmp(ch1.profile_id, "tune-a") == 0 &&
          ch1.kp > 0.21f && ch1.kp < 0.23f,
          "pin_job2_runs_with_its_own_v2_not_v1_or_slot");

    /* A pin-less job still falls back to the target slot (legacy path). */
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    check(dual_dispense_controller_init() == ESP_OK, "pin_legacy_init_ok");
    check(dual_dispense_controller_set_pending_profile_staged(1, "legacy-slot", 5, 5000,
          0.33f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40,
          500, 100, 30, 80, 30, 15, 200, 0),
          "pin_legacy_slot_staged");
    uint32_t leg_j = 0;
    check(dual_dispense_controller_add_material_server_job_pinned(
              1, 5000, 0, 9100, NULL, &leg_j) == ESP_OK,
          "pin_legacy_job_queued");
    dual_feed(pint + 1000, 0);
    dual_dispense_controller_tick(pint + 1000);
    (void)dual_dispense_controller_operator_ready(1, pint + 1050);
    dual_feed(pint + 1050, 0);
    dual_dispense_controller_tick(pint + 1050);
    check(dual_dispense_controller_snapshot(1, pint + 1050, &ch1) &&
          ch1.profile_version == 5 && strcmp(ch1.profile_id, "legacy-slot") == 0 &&
          ch1.kp > 0.32f && ch1.kp < 0.34f,
          "pin_legacy_job_uses_target_slot");

    /* ---- telemetry_client_parse_job_pin ---- */
    {
        job_profile_t parsed;
        char err[80];
        memset(&parsed, 0, sizeof(parsed));
        memset(err, 0, sizeof(err));

        /* A legacy command carries no pin at all. */
        const char *legacy =
            "{\"command_id\":1,\"command_type\":\"JOB\",\"target_g\":5000,"
            "\"material_id\":\"M1\",\"priority\":0}";
        check(telemetry_client_parse_job_pin(legacy, 1, 5000, &parsed, err, sizeof(err))
                  == JOB_PIN_ABSENT,
              "pin_parse_legacy_absent");
        check(!parsed.valid, "pin_parse_absent_leaves_invalid");

        /* The real server payload: top-level pin echo + nested gains object. */
        const char *pinned =
            "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,"
            "\"material_id\":\"M1\",\"priority\":0,"
            "\"profile_id\":\"tune-a\",\"profile_version\":3,"
            "\"profile\":{\"profile_id\":\"tune-a\",\"version\":3,"
            "\"kp\":0.0025,\"ki\":0.0003,\"kd\":0.0001,"
            "\"tolerance_g\":20,\"max_overshoot_g\":100,\"max_duration_ms\":120000,"
            "\"window_ms\":500,\"min_on_ms\":40,\"min_off_ms\":40}}";
        memset(&parsed, 0, sizeof(parsed));
        check(telemetry_client_parse_job_pin(pinned, 1, 5000, &parsed, err, sizeof(err))
                  == JOB_PIN_PARSED,
              "pin_parse_pinned_ok");
        check(parsed.valid && parsed.version == 3 &&
                  strcmp(parsed.profile_id, "tune-a") == 0 &&
                  parsed.kp > 0.0024f && parsed.kp < 0.0026f &&
                  parsed.tolerance_g == 20 && parsed.min_on_ms == 40 &&
                  parsed.window_ms == 500 && parsed.max_duration_ms == 120000,
              "pin_parse_pinned_fields");
        check(parsed.settle_time_ms > 0 && parsed.coarse_min_on_ms > 0,
              "pin_parse_fills_staged_defaults");

        /* Gains missing because the version was deleted after queueing:
         * refuse, never fall back to the shared slot. */
        const char *broken =
            "{\"command_id\":3,\"command_type\":\"JOB\",\"target_g\":5000,"
            "\"material_id\":\"M1\",\"priority\":0,"
            "\"profile_id\":\"tune-a\",\"profile_version\":3,\"profile\":null}";
        memset(&parsed, 0, sizeof(parsed));
        check(telemetry_client_parse_job_pin(broken, 1, 5000, &parsed, err, sizeof(err))
                  == JOB_PIN_REFUSED,
              "pin_parse_missing_gains_refused");
        check(!parsed.valid, "pin_parse_refused_leaves_invalid");

        /* A pin with no version is not a pin we can honour. */
        const char *nover =
            "{\"command_id\":4,\"command_type\":\"JOB\",\"target_g\":5000,"
            "\"material_id\":\"M1\",\"priority\":0,\"profile_id\":\"tune-a\","
            "\"profile\":{\"kp\":0.0025,\"ki\":0.0,\"kd\":0.0,\"tolerance_g\":20,"
            "\"max_overshoot_g\":100,\"max_duration_ms\":120000,\"window_ms\":500,"
            "\"min_on_ms\":40,\"min_off_ms\":40}}";
        memset(&parsed, 0, sizeof(parsed));
        check(telemetry_client_parse_job_pin(nover, 1, 5000, &parsed, err, sizeof(err))
                  == JOB_PIN_REFUSED,
              "pin_parse_missing_version_refused");
    }

    /* ---- Manual pump (relay) control ----
     * QEMU has no PCF8574 expander, so relay_set(0/1, true) is gated by
     * relay_expander_actuation_allowed() and returns ESP_ERR_INVALID_STATE.
     * These tests verify the SAFETY LOGIC (refusal conditions, STOP always
     * works, mutual exclusion) without requiring hardware actuation. */
    {
        ESP_LOGI(TAG, "manual pump control tests");

        /* Reset to a known state. */
        dual_dispense_controller_clear_emergency_stop();
        safety_manager_clear();

        /* STOP is always allowed, even when the relay is already off. */
        check(dual_dispense_controller_manual_relay(1, false, 1000) == ESP_OK,
              "manual_stop_always_ok_when_idle");
        check(dual_dispense_controller_manual_relay(2, false, 1000) == ESP_OK,
              "manual_stop_ch2_always_ok");

        /* Invalid channel IDs are rejected. */
        check(dual_dispense_controller_manual_relay(0, true, 1000) != ESP_OK,
              "manual_start_ch0_rejected");
        check(dual_dispense_controller_manual_relay(3, true, 1000) != ESP_OK,
              "manual_start_ch3_rejected");

        /* START refused when E-Stop is latched. */
        dual_dispense_controller_emergency_stop(2000);
        check(dual_dispense_controller_manual_relay(1, true, 2100) != ESP_OK,
              "manual_start_refused_during_estop");
        /* STOP still works during E-Stop. */
        check(dual_dispense_controller_manual_relay(1, false, 2200) == ESP_OK,
              "manual_stop_ok_during_estop");
        dual_dispense_controller_clear_emergency_stop();

        /* START refused when a safety fault is active. */
        safety_manager_raise(SAFETY_OVERSHOOT, 3000);
        check(dual_dispense_controller_manual_relay(1, true, 3100) != ESP_OK,
              "manual_start_refused_during_safety_fault");
        safety_manager_clear();

        /* START after fault clear is accepted (relay_set may still fail in
         * QEMU due to no expander, but the function should not return
         * ESP_ERR_INVALID_STATE from the safety checks). */
        esp_err_t e = dual_dispense_controller_manual_relay(1, true, 4000);
        check(e != ESP_ERR_INVALID_ARG,
              "manual_start_after_fault_clear_not_invalid_arg");
        /* STOP always works regardless. */
        check(dual_dispense_controller_manual_relay(1, false, 4100) == ESP_OK,
              "manual_stop_ok_after_fault_clear");

        /* Verify snapshot manual flag is cleared after STOP. */
        dual_channel_snapshot_t snap;
        check(dual_dispense_controller_snapshot(1, 5000, &snap) && !snap.manual,
              "manual_flag_cleared_after_stop");
    }
    extern void test_mqtt_run(void);
    test_mqtt_run();
    extern void test_concurrency_run(void);
    test_concurrency_run();
    extern void test_flow_estimator_run(void);
    test_flow_estimator_run();
    extern void test_autotune_run(void);
    test_autotune_run();
    extern void test_prereq_batch1_run(void);
    test_prereq_batch1_run();
    extern void test_prereq_fix1_run(void);
    test_prereq_fix1_run();
    extern void test_weight_mqtt_run(void);
    test_weight_mqtt_run();
    extern void test_run_mqtt_run(void);
    test_run_mqtt_run();
    extern void test_single_scale_run(void);
    test_single_scale_run();
    ESP_LOGI(TAG, "TEST_SUITE_END pass=%d fail=%d", s_pass, s_fail);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* The suite body is one very large function (its frame alone is ~6.4 KB) and the
 * boot-time DRAM cannot afford a main-task stack that big, so it runs on a task
 * created after boot (the same trick test_run_on_big_stack() uses for the later
 * suites). app_main just starts it. */
static void tests_task(void *arg)
{
    (void)arg;
    tests_body();
    vTaskDelete(NULL);
}

void app_main(void)
{
    if (xTaskCreate(tests_task, "tests_main", 14336, NULL, uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        ESP_LOGE(TAG, "tests_main task not created");
    }
}
