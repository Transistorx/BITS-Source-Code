#ifndef RELAY_DRIVER_H
#define RELAY_DRIVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Logical relay channels, zero-based.
 *
 * CONFIRMED HARDWARE: Relay 1 and Relay 2 — the coarse and fine dosing valves
 * — are outputs P6 and P7 of a PCF8574T at 7-bit address 0x22. They are NOT
 * direct ESP32 GPIOs and are never driven as such while that backend is
 * active. See relay_expander.h for the address derivation, the bit masks and
 * the evidence-based active-level determination.
 *
 * The AO1..AO12 GPIO table below is the REFERENCE-SCHEMATIC mapping. It is
 * kept as the direct-GPIO fallback backend (board variants that drive the AO
 * nets, and QEMU which has no I2C slaves) and for channels 3..12, whose
 * outputs on this board are not on the confirmed expander.
 *
 * AO12 / GPIO2 is in that table, and on this project the status LED is
 * disabled (CONFIG_LED_STATUS_GPIO=-1) so relay_driver is the pin's single
 * owner. */
#define RELAY_COUNT 12U

/* Relay 1 = coarse/high-flow, Relay 2 = fine/low-flow. Both live on the
 * confirmed expander. */
#define RELAY_INDEX_COARSE 0U
#define RELAY_INDEX_FINE   1U

/* Schematic: GPIO -> 4.7k -> S8050 NPN -> G3MB-202PL SSR. HIGH energizes. */
#define RELAY_ACTIVE_LEVEL   1
#define RELAY_INACTIVE_LEVEL 0

/* Board resources (reserved, do not repurpose) — confirmed from the PCB
 * schematic, unused by this firmware:
 *   SDA = GPIO22, SCL = GPIO23 : PCF8574 I2C expanders carrying the 16
 *                                digital inputs (NOT direct ESP32 GPIOs)
 *   LRX = GPIO16, LTX = GPIO17 : RS232 serial interface
 *   A1  = GPIO36, A2  = GPIO39 : analog inputs (input-only pads, ADC1)
 * Strapping pins among the AO channels: GPIO2 (AO12) and GPIO12 (AO11).
 * relay_init_all() pre-drives every AO LOW before switching pads to output
 * and enables no pulls, so firmware never forces an invalid boot level;
 * straps are sampled at reset only, so relay_all_on() at runtime cannot
 * affect a subsequent boot. NOTE: GPIO2/4/12/13/25/26/27 are ADC2 channels —
 * ADC2 is unusable while Wi-Fi is active; do not add analog monitoring on
 * AO-adjacent pins. */

/*
 * ENCAPSULATION BOUNDARY: relay_set / relay_all_on / relay_all_off /
 * relay_get_state are the ONLY relay operations the rest of the firmware may
 * call. The job queue, the dispense state machine, the PID and the windowed
 * time-proportional control never see an expander address, an expander bit or
 * a raw byte — every PCF8574 detail stops inside relay_driver/relay_expander.
 */
esp_err_t relay_init_all(void);
esp_err_t relay_set(uint8_t index, bool on);

/* Group commands: every channel 1..12 as one logical group. Per-channel cache
 * checks make repeated group commands cheap, and transition logs only fire on
 * an actual change. */
esp_err_t relay_all_on(void);
esp_err_t relay_all_off(void);

/* H1: UNCONDITIONAL all-OFF for fault paths. Writes the OFF pattern to the
 * expander whatever the shadow/cache say and confirms it by read-back; a failure
 * is returned and the state stays UNKNOWN (relay_get_state() reports ON). */
esp_err_t relay_force_all_off(void);

/* True when an ON command would be passed to the hardware. False only when the
 * I2C backend is active and ON is suppressed by the verification gate
 * (zero bus traffic). The ONLY way to tell a gated refusal from a real write
 * failure: error codes cannot (ESP_ERR_INVALID_STATE also means a stuck bus). */
bool relay_actuation_allowed(void);

/* Reports ON while the true state is unknown (after a failed write) so callers
 * never treat a latched-but-unconfirmed relay as OFF. */
bool relay_get_state(uint8_t index);

/* True when Relay 1 / Relay 2 are served by the confirmed I2C expander;
 * false = the direct-GPIO fallback is driving them. Diagnostics use this to
 * word their logs truthfully. */
bool relay_backend_is_i2c(void);

/* Human-readable location of a channel's output, for diagnostics — e.g.
 * "expander 0x22 bit6 (ack=1)" or "GPIO21". The expander's address and bit
 * numbering stay inside the driver: callers above this line compare strings,
 * they never see or construct a PCF8574 bit. Returns the number of characters
 * written (excluding the NUL), or 0 for a bad index or buffer. */
int relay_describe_channel(uint8_t index, char *buf, size_t cap);

/* User-attended expander diagnostics, re-exported so nothing but the driver
 * needs to know the expander module exists. Both no-op with an ERROR log
 * unless the confirmed expander is active. Kconfig-gated in app_main and
 * requiring MAINS DISCONNECTED. */
void relay_backend_run_map_test(void);
void relay_backend_run_pcf_test(void);
void relay_backend_run_act_test(void);

/* Pure lookup: GPIO for a zero-based index, or -1 when out of range. */
int relay_gpio_for_index(uint8_t index);

/*
 * Reads the actual pad level back. HARDWARE ONLY: QEMU does not emulate GPIO
 * output readback, so under the QEMU test suite this always reads inactive
 * whatever was written. Use it on the bench to confirm the pad follows the
 * cached commanded state.
 */
bool relay_pad_is_energized(uint8_t index);

#endif /* RELAY_DRIVER_H */
