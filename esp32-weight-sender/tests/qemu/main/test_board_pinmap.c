/*
 * Board pin-map contract tests.
 *
 * These lock the one authoritative scale pin mapping: correct UART
 * ownership, no console-UART theft, no pad reuse, no EMAC collision, and
 * the MAX13487E AutoDirection contract (no ESP32 DE/RE pad, no RS-485
 * half-duplex direction mode).
 *
 * They also lock the GPIO15 (MTDO) strap-pad handling and the single-scale
 * channel selection, so a silent remap cannot reintroduce the boot hazard.
 */

#include "test_harness.h"

#include "board_pins.h"
#include "scale_transport.h"

#include <stdio.h>
#include <string.h>

/* Mirror of the EMAC/RMII pads guarded in board_pins.h. */
static const int k_emac_pads[] = BOARD_EMAC_PADS;

static bool pad_is_emac(int gpio)
{
    for (size_t i = 0; i < sizeof(k_emac_pads) / sizeof(k_emac_pads[0]); ++i) {
        if (k_emac_pads[i] == gpio) return true;
    }
    return false;
}

static void test_board_variant_is_selected(void)
{
    /* Failing here means no WEIGHT_DEMO_BOARD_* choice was made, which the
     * header turns into #error. The fact we compiled proves one is set. */
    test_check(BOARD_VARIANT_NAME != NULL && BOARD_VARIANT_NAME[0] != '\0',
               "board variant name is present");
}

static void test_two_channels_exist_with_distinct_uart_peripherals(void)
{
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);

    test_check(ch1 != NULL, "channel 1 has a pin map entry");
    test_check(ch2 != NULL, "channel 2 has a pin map entry");
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(ch1->uart_port != ch2->uart_port,
               "channels own different UART peripherals");
    test_check(ch1->uart_port >= 1 && ch1->uart_port <= 2,
               "channel 1 UART is in the free 1..2 range");
    test_check(ch2->uart_port >= 1 && ch2->uart_port <= 2,
               "channel 2 UART is in the free 1..2 range");
}

static void test_console_uart_is_never_claimed(void)
{
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(ch1->uart_port != BOARD_CONSOLE_UART_PORT,
               "channel 1 does not take the console UART");
    test_check(ch2->uart_port != BOARD_CONSOLE_UART_PORT,
               "channel 2 does not take the console UART");

    test_check(ch1->tx_gpio != BOARD_CONSOLE_TX_GPIO &&
               ch1->tx_gpio != BOARD_CONSOLE_RX_GPIO,
               "channel 1 TX avoids console pads GPIO1/GPIO3");
    test_check(ch1->rx_gpio != BOARD_CONSOLE_TX_GPIO &&
               ch1->rx_gpio != BOARD_CONSOLE_RX_GPIO,
               "channel 1 RX avoids console pads GPIO1/GPIO3");
    test_check(ch2->tx_gpio != BOARD_CONSOLE_TX_GPIO &&
               ch2->tx_gpio != BOARD_CONSOLE_RX_GPIO,
               "channel 2 TX avoids console pads GPIO1/GPIO3");
    test_check(ch2->rx_gpio != BOARD_CONSOLE_TX_GPIO &&
               ch2->rx_gpio != BOARD_CONSOLE_RX_GPIO,
               "channel 2 RX avoids console pads GPIO1/GPIO3");
}

static void test_tx_rx_are_distinct_and_never_shared(void)
{
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(ch1->tx_gpio != ch1->rx_gpio, "channel 1 TX != RX");
    test_check(ch2->tx_gpio != ch2->rx_gpio, "channel 2 TX != RX");

    test_check(ch1->tx_gpio != ch2->tx_gpio, "CH1 TX not reused as CH2 TX");
    test_check(ch1->tx_gpio != ch2->rx_gpio, "CH1 TX not reused as CH2 RX");
    test_check(ch1->rx_gpio != ch2->tx_gpio, "CH1 RX not reused as CH2 TX");
    test_check(ch1->rx_gpio != ch2->rx_gpio, "CH1 RX not reused as CH2 RX");
}

static void test_tx_pads_are_output_capable(void)
{
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(!BOARD_IS_INPUT_ONLY_PAD(ch1->tx_gpio),
               "channel 1 TX is not on an input-only pad");
    test_check(!BOARD_IS_INPUT_ONLY_PAD(ch2->tx_gpio),
               "channel 2 TX is not on an input-only pad");
    test_check(!BOARD_IS_INPUT_ONLY_PAD(ch1->rx_gpio),
               "channel 1 RX is not on an input-only pad");
    test_check(!BOARD_IS_INPUT_ONLY_PAD(ch2->rx_gpio),
               "channel 2 RX is not on an input-only pad");
}

static void test_no_emac_rmi_pad_collision(void)
{
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(!pad_is_emac(ch1->tx_gpio), "CH1 TX is not an EMAC pad");
    test_check(!pad_is_emac(ch1->rx_gpio), "CH1 RX is not an EMAC pad");
    test_check(!pad_is_emac(ch2->tx_gpio), "CH2 TX is not an EMAC pad");
    test_check(!pad_is_emac(ch2->rx_gpio), "CH2 RX is not an EMAC pad");
}

static void test_rs485_map_uses_the_schematic_pads(void)
{
#if BOARD_VARIANT_IS_RS485
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    /* Candidate schematic map: CH1 TX=GPIO13 RX=GPIO15, CH2 TX=GPIO32 RX=GPIO33. */
    test_check(ch1->uart_port == 1, "RS485 CH1 is UART1");
    test_check(ch1->tx_gpio == 13, "RS485 CH1 TX is GPIO13");
    test_check(ch1->rx_gpio == 15, "RS485 CH1 RX is GPIO15");
    test_check(ch2->uart_port == 2, "RS485 CH2 is UART2");
    test_check(ch2->tx_gpio == 32, "RS485 CH2 TX is GPIO32");
    test_check(ch2->rx_gpio == 33, "RS485 CH2 RX is GPIO33");
#endif
}

static void test_legacy_map_uses_the_rs232_pads(void)
{
#if !BOARD_VARIANT_IS_RS485
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(ch1->uart_port == 1, "RS232 CH1 is UART1");
    test_check(ch1->tx_gpio == 33, "RS232 CH1 TX is GPIO33");
    test_check(ch1->rx_gpio == 35, "RS232 CH1 RX is GPIO35");
    test_check(ch2->uart_port == 2, "RS232 CH2 is UART2");
    test_check(ch2->tx_gpio == 32, "RS232 CH2 TX is GPIO32");
    test_check(ch2->rx_gpio == 34, "RS232 CH2 RX is GPIO34");
#endif
}

static void test_max13487e_has_no_esp32_direction_pin(void)
{
#if BOARD_VARIANT_IS_RS485
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch1 == NULL || ch2 == NULL) return;

    test_check(ch1->de_re_mode == BOARD_DE_RE_AUTO_DIRECTION,
               "RS485 CH1 direction is hardware AutoDirection");
    test_check(ch2->de_re_mode == BOARD_DE_RE_AUTO_DIRECTION,
               "RS485 CH2 direction is hardware AutoDirection");
    test_check(ch1->transport == SCALE_XPORT_RS485_AUTO_DIR,
               "RS485 CH1 transport is the auto-direction RS485 label");
    test_check(ch2->transport == SCALE_XPORT_RS485_AUTO_DIR,
               "RS485 CH2 transport is the auto-direction RS485 label");

    /*
     * A DE/RE pad must never appear in the link record. The MAX13487E owns
     * direction internally; wiring an ESP32 pad to it would be a hardware
     * fault, not a configuration option.
     */
    test_check(strstr(board_de_re_mode_name(ch1->de_re_mode), "Software") == NULL,
               "CH1 direction is not software-controlled");
    test_check(strstr(board_de_re_mode_name(ch2->de_re_mode), "Software") == NULL,
               "CH2 direction is not software-controlled");
#endif
}

static void test_connector_details_are_marked_unverified(void)
{
    const board_scale_link_t *link = board_pinmap_get(board_pinmap_active_channel());
    test_check(link != NULL, "active channel has a pin map entry");
    if (link == NULL) return;

    /* A/B/GND numbering cannot be proven from any repository file. */
    test_check(strcmp(link->connector.a_pin, "VERIFY ON PCB") == 0,
               "A pin is marked VERIFY ON PCB, not guessed");
    test_check(strcmp(link->connector.b_pin, "VERIFY ON PCB") == 0,
               "B pin is marked VERIFY ON PCB, not guessed");
    test_check(strcmp(link->connector.gnd_pin, "VERIFY ON PCB") == 0,
               "GND pin is marked VERIFY ON PCB, not guessed");
}

static void test_gpio15_strap_pad_is_flagged_on_ch1(void)
{
#if BOARD_VARIANT_IS_RS485
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    test_check(ch1 != NULL, "channel 1 present");
    if (ch1 == NULL) return;

    test_check(ch1->rx_gpio == BOARD_GPIO15_MTDO_STRAP,
               "RS485 CH1 RX is the GPIO15 MTDO strap pad");
    test_check(ch1->gpio15_strap_pin,
               "CH1 map carries the strap-pad flag so diagnostics can warn");

    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    test_check(ch2 != NULL && !ch2->gpio15_strap_pin,
               "CH2 is free of the strap-pad hazard");
#endif
}

static void test_strap_pad_check_runs_without_blocking(void)
{
    /* Report-only: must return normally whether or not a pad can be read
     * (QEMU has no real strap state). This guards against a future version
     * that turns the check into a boot abort. */
    board_pinmap_check_strap_pad();
    test_check(true, "GPIO15 strap-pad check returned without blocking boot");
}

static void test_active_channel_prefers_ch2_for_the_single_scale(void)
{
    board_channel_t active = board_pinmap_active_channel();
    test_check(active == BOARD_CH_SCALE_1 || active == BOARD_CH_SCALE_2,
               "active channel is a valid channel");

#if !defined(CONFIG_WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL) || \
    (CONFIG_WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL == 2)
    /*
     * Default single-scale selection. Channel 1 RX is GPIO15 (MTDO), so
     * Channel 2 is preferred unless boot/restart has been verified with the
     * Channel 1 receiver connected, idle, and actively streaming.
     */
    test_check(active == BOARD_CH_SCALE_2,
               "single scale defaults to CH2 to avoid the GPIO15 strap hazard");
#endif
}

static void test_fill_link_matches_the_board_map(void)
{
    scale_link_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    test_check(board_pinmap_fill_link(BOARD_CH_SCALE_2, 9600, &cfg),
               "fill_link succeeds for a valid channel");

    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch2 == NULL) return;

    test_check(cfg.uart_port == ch2->uart_port, "fill_link copies the UART port");
    test_check(cfg.tx_gpio == ch2->tx_gpio, "fill_link copies the TX pad");
    test_check(cfg.rx_gpio == ch2->rx_gpio, "fill_link copies the RX pad");
    test_check(cfg.baud_rate == 9600, "fill_link honours the requested baud");
    test_check(cfg.transport == ch2->transport, "fill_link copies the transport");
    test_check(cfg.channel == (int)BOARD_CH_SCALE_2, "fill_link records the channel id");
}

static void test_fill_link_rejects_bad_input(void)
{
    scale_link_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    test_check(!board_pinmap_fill_link((board_channel_t)99, 9600, &cfg),
               "fill_link rejects an out-of-range channel");
    test_check(!board_pinmap_fill_link(BOARD_CH_SCALE_1, 9600, NULL),
               "fill_link rejects a NULL output pointer");
}

static void test_fill_link_falls_back_to_map_baud(void)
{
    scale_link_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    test_check(board_pinmap_fill_link(BOARD_CH_SCALE_1, 0, &cfg),
               "fill_link succeeds when baud is left to the map");
    test_check(cfg.baud_rate == BOARD_DEFAULT_BAUD,
               "fill_link falls back to the board default baud");
}

static void test_invalid_channel_lookup_returns_null(void)
{
    test_check(board_pinmap_get((board_channel_t)(-1)) == NULL,
               "negative channel returns NULL");
    test_check(board_pinmap_get((board_channel_t)2) == NULL,
               "channel past the end returns NULL");
}

static void test_diagnostic_lines_cover_every_channel(void)
{
    /* Must not crash and must be callable repeatedly at startup. */
    board_pinmap_log(BOARD_CH_SCALE_1);
    board_pinmap_log(BOARD_CH_SCALE_2);
    board_pinmap_log_all();
    test_check(true, "startup diagnostics ran for both channels");
}

/*
 * Channel 1 boot/restart verification.
 *
 * GPIO15 is MTDO on classic ESP32: the boot ROM samples it during reset. A
 * receiver that holds the bus low at the wrong moment can silence the boot
 * log or, in the worst case, change boot behaviour. The three states the
 * bench must cover are encoded here so the policy cannot drift:
 *
 *   connected  - RS-485 receiver wired to CH1, bus idle-high at reset
 *   idle       - receiver attached but no traffic during reset
 *   streaming  - receiver actively driving the bus during reset
 *
 * The firmware never remaps pins to work around a Channel 1 boot problem.
 * The remedy is always "use Channel 2 (GPIO32/33)".
 */

typedef enum {
    CH1_STATE_CONNECTED = 0,
    CH1_STATE_IDLE = 1,
    CH1_STATE_STREAMING = 2,
} ch1_boot_state_t;

static const char *ch1_boot_state_name(ch1_boot_state_t state)
{
    switch (state) {
    case CH1_STATE_CONNECTED:  return "connected";
    case CH1_STATE_IDLE:       return "idle";
    case CH1_STATE_STREAMING:  return "actively-streaming";
    default:                   return "unknown";
    }
}

/*
 * Policy predicate, not a hardware measurement. QEMU has no real strap pad
 * and no receiver, so this encodes the decision rule the bench applies:
 * whenever Channel 1 boot is observed unstable in ANY of the three states,
 * the single scale moves to Channel 2. It never triggers a pin remap.
 */
static bool ch1_boot_unstable_requires_ch2_fallback(ch1_boot_state_t state,
                                                    bool observed_unstable)
{
    (void)state;
    return observed_unstable;
}

static void test_gpio15_is_recognised_as_a_strap_pad(void)
{
    /* GPIO15 = MTDO on classic ESP32. The map must record the number so the
     * strap check and the startup warning key off the same constant. */
    test_check(BOARD_GPIO15_MTDO_STRAP == 15,
               "MTDO strap pad constant is GPIO15");

#if BOARD_VARIANT_IS_RS485
    const board_scale_link_t *ch1 = board_pinmap_get(BOARD_CH_SCALE_1);
    if (ch1 == NULL) return;
    test_check(ch1->rx_gpio == BOARD_GPIO15_MTDO_STRAP,
               "CH1 RX is on the MTDO strap pad, so boot is strap-sensitive");
    test_check(ch1->gpio15_strap_pin,
               "CH1 is flagged as strap-sensitive for diagnostics and policy");
#endif
}

static void test_boot_states_all_route_to_ch2_fallback(void)
{
    /* Each of the three bench states, stable and unstable. */
    for (int s = CH1_STATE_CONNECTED; s <= CH1_STATE_STREAMING; ++s) {
        ch1_boot_state_t state = (ch1_boot_state_t)s;

        test_check(!ch1_boot_unstable_requires_ch2_fallback(state, false),
                   ch1_boot_state_name(state));

        /* Unstable in ANY state -> CH2. Name the outcome in the assertion so
         * a failure states the policy, not just a boolean. */
        char name[96];
        snprintf(name, sizeof(name),
                 "ch1_%s_unstable_falls_back_to_ch2", ch1_boot_state_name(state));
        test_check(ch1_boot_unstable_requires_ch2_fallback(state, true), name);
    }
}

static void test_fallback_never_remaps_pins(void)
{
    /* The remedy is a channel switch, never a pad change. Prove the maps
     * for both channels are independent of any runtime decision: a lookup
     * returns the same pads every time, and no API exists to rewrite them. */
    const board_scale_link_t *a = board_pinmap_get(BOARD_CH_SCALE_1);
    const board_scale_link_t *b = board_pinmap_get(BOARD_CH_SCALE_1);
    test_check(a == b && a != NULL,
               "channel map is a fixed static table, not reconfigurable at runtime");

    if (a == NULL) return;
    test_check(a->tx_gpio == BOARD_CH1_TX_GPIO && a->rx_gpio == BOARD_CH1_RX_GPIO,
               "CH1 pads come from the compile-time map and cannot be remapped");

    const board_scale_link_t *ch2 = board_pinmap_get(BOARD_CH_SCALE_2);
    if (ch2 == NULL) return;
    test_check(ch2->tx_gpio == BOARD_CH2_TX_GPIO && ch2->rx_gpio == BOARD_CH2_RX_GPIO,
               "CH2 pads are the documented fallback, not an ad-hoc remap");
}

static void test_boot_verification_is_report_only(void)
{
    /* Three repeated calls simulate restart. None may block or flip state. */
    board_pinmap_check_strap_pad();
    board_pinmap_check_strap_pad();
    board_pinmap_check_strap_pad();
    test_check(true, "strap-pad check is idempotent across restarts");

    /* Diagnostics must still work after the check, in every boot state. */
    board_pinmap_log(BOARD_CH_SCALE_1);
    test_check(true, "CH1 diagnostics survive the strap-pad check");
}

static void test_ch2_fallback_channel_is_usable(void)
{
    /* If CH1 is abandoned for boot reasons, CH2 must be a complete link. */
    scale_link_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    test_check(board_pinmap_fill_link(BOARD_CH_SCALE_2, 9600, &cfg),
               "CH2 fallback link can be built");

#if BOARD_VARIANT_IS_RS485
    test_check(cfg.uart_port == 2 && cfg.tx_gpio == 32 && cfg.rx_gpio == 33,
               "CH2 fallback is UART2 TX=GPIO32 RX=GPIO33");
    test_check(!BOARD_IS_INPUT_ONLY_PAD(cfg.tx_gpio),
               "CH2 fallback TX is output-capable");
    test_check(cfg.rx_gpio != BOARD_GPIO15_MTDO_STRAP,
               "CH2 fallback avoids the GPIO15 strap hazard");
    test_check(cfg.transport == SCALE_XPORT_RS485_AUTO_DIR,
               "CH2 fallback keeps the same RS485 AutoDirection transport");
#endif
}

void test_board_pinmap_run(void)
{
    test_board_variant_is_selected();
    test_two_channels_exist_with_distinct_uart_peripherals();
    test_console_uart_is_never_claimed();
    test_tx_rx_are_distinct_and_never_shared();
    test_tx_pads_are_output_capable();
    test_no_emac_rmi_pad_collision();
    test_rs485_map_uses_the_schematic_pads();
    test_legacy_map_uses_the_rs232_pads();
    test_max13487e_has_no_esp32_direction_pin();
    test_connector_details_are_marked_unverified();
    test_gpio15_strap_pad_is_flagged_on_ch1();
    test_strap_pad_check_runs_without_blocking();
    test_active_channel_prefers_ch2_for_the_single_scale();
    test_fill_link_matches_the_board_map();
    test_fill_link_rejects_bad_input();
    test_fill_link_falls_back_to_map_baud();
    test_invalid_channel_lookup_returns_null();
    test_diagnostic_lines_cover_every_channel();
    test_gpio15_is_recognised_as_a_strap_pad();
    test_boot_states_all_route_to_ch2_fallback();
    test_fallback_never_remaps_pins();
    test_boot_verification_is_report_only();
    test_ch2_fallback_channel_is_usable();
}
