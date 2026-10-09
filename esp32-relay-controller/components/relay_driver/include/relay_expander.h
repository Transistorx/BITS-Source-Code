#ifndef RELAY_EXPANDER_H
#define RELAY_EXPANDER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * PCF8574T relay-expander backend — CONFIRMED HARDWARE.
 *
 * The board's relay outputs are NOT direct ESP32 GPIOs. Relay 1 and Relay 2
 * are two outputs of a single PCF8574T I2C GPIO expander:
 *
 *     7-bit I2C address 0x22   (A2=0, A1=1, A0=0 -> 0b0100_010)
 *     Relay 1 = P6   bit 6   mask 0x40   IC pin 11
 *     Relay 2 = P7   bit 7   mask 0x80   IC pin 12
 *
 * The remaining six pins (P0..P5) are NOT relay outputs on this board. They
 * belong to whatever else shares the expander, so every write here is a
 * shadow-preserving read-modify-write: the unused bits are taken from a live
 * bus read and handed back unchanged. A blind byte write would clobber them.
 *
 * RELAY 1 IS THE COARSE/HIGH-FLOW VALVE AND RELAY 2 THE FINE/LOW-FLOW VALVE
 * (zero-based indices 0 and 1), matching the dispensing architecture.
 *
 * ACTIVE LEVEL — MEASURED ON THE PHYSICAL BOARD: **ACTIVE-HIGH**.
 *   Observed directly on the bench: with the firmware shipping ACTIVE-LOW, the
 *   relay was ON whenever it should have been OFF and OFF whenever it should
 *   have been ON — a complete inversion. That is the signature of an
 *   ACTIVE-HIGH drive stage being written with active-LOW levels. Corrected
 *   and shipped as ACTIVE_HIGH.
 *
 *   OFF = bit CLEARED (0). ON = bit SET (1).
 *
 *   WHY THE EARLIER DERIVATION WAS WRONG — kept on the record, because the
 *   reasoning was plausible and someone will reach for it again. It argued:
 *     (1) a PCF8574 output is quasi-bidirectional with a weak (~100 uA)
 *         pull-up, so it sinks but cannot source, therefore any drive stage
 *         must be sink-driven and the energized level must be LOW; and
 *     (2) the device powers up with every port HIGH, so an active-HIGH board
 *         would energize its relays before the MCU booted.
 *   Both steps are sound in the abstract and both failed here: the board's
 *   drive stage evidently does not hang directly off the expander pin, and the
 *   power-up implication in (2) is real but was apparently accepted by the
 *   board's designer. The lesson is that (1) was an inference about a circuit
 *   nobody had traced, and (2) assumed a design norm rather than the one in
 *   front of us. A measurement beats a derivation; it is recorded here as such
 *   rather than quietly deleted.
 *
 *   CONSEQUENCE FOR POWER-UP (unavoidable, and a hardware matter): the PCF8574
 *   powers up with every port HIGH, which under ACTIVE-HIGH is the ENERGIZED
 *   level. Until the early OFF write lands, the relays are ON. Firmware closes
 *   that window as early as it can (see relay_expander_init) but cannot make it
 *   zero; removing it needs a hardware mitigation, e.g. pull-downs on the
 *   relay-drive inputs. And if the expander never ACKs there is no way to clear
 *   it at all — report_expander_unreachable() says so at ERROR level.
 *
 *   OPEN QUESTION, and it matters for the bench: the inversion was observed
 *   while the firmware was ALSO looking for the expander at the wrong address
 *   (0x25, corrected to 0x22 in the same change). With nothing ACKing at 0x25
 *   the backend disabled itself and relay_set() fell back to the DIRECT-GPIO
 *   path on GPIO18/19 — so the relays that moved may have been driven by that
 *   path, not by this expander at all. If so the inversion is a property of the
 *   GPIO drive stage (also active-LOW) rather than of the expander.
 *
 *   The ACTIVE-HIGH setting here is therefore the best-supported reading of the
 *   evidence, not a closed measurement of this path. What closes it is the
 *   bench: with the corrected address the backend should come up on I2C, and
 *   the boot log says which path is live. Flash, read the backend line, then
 *   with MAINS DISCONNECTED run MAPTEST or queue a ramp job and note which
 *   relay clicks in which phase. See the README for the exact lines to read.
 *   The Kconfig choice and the MAPTEST/PCF_TEST paths exist so that answer can
 *   flip the setting without touching code.
 *
 * SAFETY DESIGN:
 *  - Discovery is READ-ONLY. The only writes are the OFF pattern (always
 *    allowed — see below) and, once verified, ON commands.
 *  - Chip gate: a PCF8574 has no registers and no pointer auto-increment, so
 *    a two-byte plain read repeats the same pin state (b0 == b1). b0 != b1
 *    means a register device (MCP23017/PCA9555) is at that address and
 *    PCF8574-style byte writes would be chaotic — the backend then disables
 *    itself with an ERROR rather than writing.
 *  - The shadow is seeded from a live bus read, never blind.
 *  - Every physical write is logged with its esp_err result.
 *
 * ENCAPSULATION: this module is the ONLY place in the firmware that knows the
 * PCF8574's address, its bits or its byte layout. relay_driver exposes the
 * logical relay_set()/relay_all_on()/relay_all_off() surface, and the job
 * queue, the dispense state machine and the PID/windowed control never see a
 * raw expander bit.
 */

/* Base address of the non-A PCF8574 = 0b0100_000 = 0x20; the A variant
 * (PCF8574A) is 0x38. This is the BASE only - A2/A1/A0 are OR-ed in below.
 * It must not be pre-loaded with strap bits, or the OR would silently absorb
 * them and the address would look right while the derivation was wrong. */
#define RELAY_EXPANDER_BASE_ADDR 0x20U

/* Address-select strapping, MEASURED on the physical board: A2=0, A1=1, A0=0.
 *
 * This SUPERSEDES the earlier 0x25 confirmation (A2=1, A1=0, A0=1), which was
 * the exact inverse. Measurement beat the earlier report here for the same
 * reason it beat the polarity derivation: a reading taken off the running board
 * outranks an inference about it. The A2/A1/A0 form is kept rather than the
 * literal so the derivation stays visible and stays pinned by the test. */
#define RELAY_EXPANDER_A2 0U
#define RELAY_EXPANDER_A1 1U
#define RELAY_EXPANDER_A0 0U

/* 0b0100_A2A1A0 = 0b0100010 = 0x22. */
#define RELAY_EXPANDER_ADDR \
    (RELAY_EXPANDER_BASE_ADDR | (RELAY_EXPANDER_A2 << 2) | \
     (RELAY_EXPANDER_A1 << 1) | (RELAY_EXPANDER_A0))

#define RELAY_EXPANDER_BIT_RELAY1  6U  /* P6, IC pin 11 */
#define RELAY_EXPANDER_BIT_RELAY2  7U  /* P7, IC pin 12 */
#define RELAY_EXPANDER_MASK_RELAY1 (1U << RELAY_EXPANDER_BIT_RELAY1) /* 0x40 */
#define RELAY_EXPANDER_MASK_RELAY2 (1U << RELAY_EXPANDER_BIT_RELAY2) /* 0x80 */

/* Only P6 and P7 are relay outputs; every other bit is preserved verbatim. */
#define RELAY_EXPANDER_USED_MASK \
    (RELAY_EXPANDER_MASK_RELAY1 | RELAY_EXPANDER_MASK_RELAY2)         /* 0xC0 */

/*
 * Read-only discovery + chip gate, then the EARLY OFF WRITE. Safe to call
 * unconditionally: creates the ESP-IDF v6.1 i2c_master bus (I2C_NUM_0, SDA/SCL
 * from Kconfig, 100 kHz), scans 0x08..0x77 logging every ACK, attaches 0x22,
 * chip-gates it and seeds the shadow from a live read. Tolerates every failure
 * (no bus, no ACK, QEMU) with ERROR logs; NEVER aborts boot. Idempotent.
 */
esp_err_t relay_expander_init(void);

/* The confirmed expander was found AND passed the chip gate. */
bool relay_expander_backend_active(void);
bool relay_expander_detected(void);

/* Configured polarity. Default is ACTIVE-HIGH - MEASURED on the board, not
 * derived; the Kconfig choice exists so a board variant can invert it. */
bool relay_expander_active_low(void);

/*
 * Are ON commands allowed? Requires the mapping/polarity to be field-verified
 * (WEIGHT_DEMO_POLARITY_VERIFIED). OFF commands are NOT gated — see
 * relay_expander_all_off().
 */
bool relay_expander_actuation_allowed(void);

/* ---- Actuation (relay_driver calls these under its own serialization; this
 * module keeps no internal lock). Transition-only: a shadow that already
 * matches produces ZERO bus traffic, which is what keeps the 250 ms failsafe
 * re-assertion silent on the bus. ---- */

/* One logical channel, indices 0 (Relay 1) and 1 (Relay 2) only. */
esp_err_t relay_expander_set_channel(uint8_t index, bool on);

/*
 * Drives the relay bits to the INACTIVE level.
 *
 * ALWAYS ALLOWED, even before verification, and deliberately not gated like the
 * ON path — because de-energizing is the direction every fault path depends on
 * and must never be refused.
 *
 * Under the MEASURED active-HIGH polarity the inactive level is bit CLEARED.
 * On this board that write is load-bearing rather than a formality: the
 * PCF8574 powers up with every port HIGH, which is the ENERGIZED level here, so
 * this call is what actually switches the relays OFF. It is therefore issued as
 * early as the firmware can manage (relay_expander_init, from relay_init_all,
 * before Wi-Fi / web API / supervisor) and again on every fault path.
 *
 * It is still safe to issue unconditionally: clearing a bit that is already
 * clear is a no-op, and clearing a bit that is set can only ever de-energize.
 */
esp_err_t relay_expander_all_off(void);

/*
 * H1: UNCONDITIONAL OFF. Writes the OFF pattern whatever the shadow says, then
 * reads the port back and confirms the relay bits (see the .c for what a
 * read-back proves on a quasi-bidirectional port). A failed write, a failed read
 * or a mismatch returns an error and leaves the state UNKNOWN. Never gated.
 */
esp_err_t relay_expander_force_all_off(void);

/* H1: true after any failed write until a write ACKs. While true the shadow does
 * not prove the wire state and OFF commands are never short-circuited. */
bool relay_expander_state_unknown(void);

/* Drives the relay bits to the ENERGIZED level. Gated on verification. */
esp_err_t relay_expander_all_on(void);

/* Shadow-derived state: is this channel's bit at the energized level? */
bool relay_expander_channel_on(uint8_t index);

/* True when the last physical write carrying this channel ACKed. On the I2C
 * backend there is no pad readback; relay_pad_is_energized() reports this. */
bool relay_expander_last_write_ok(uint8_t index);

/* The live shadow byte (diagnostics and tests). */
uint8_t relay_expander_shadow(void);

/* ---- Pure helpers (no bus, no state) — unit-tested in the QEMU suite. ---- */

/* Logical index -> expander bit. True only for 0 (P6) and 1 (P7). */
bool relay_expander_bit_for_index(uint8_t index, uint8_t *bit);

/* Return `shadow` with one bit moved to the wire level that means `on` under
 * the given polarity (under ACTIVE-LOW, on -> bit cleared). */
uint8_t relay_expander_shadow_set(uint8_t shadow, uint8_t bit, bool on,
                                  bool active_low);

/* Return `shadow` with every relay bit at the `on` wire level; the six unused
 * bits are preserved verbatim. This is the group command and the boot
 * all-INACTIVE pattern (on = false). */
uint8_t relay_expander_shadow_all(uint8_t shadow, bool on, bool active_low);

/* Is this shadow bit at the energized wire level for the polarity? */
bool relay_expander_bit_is_on(uint8_t shadow, uint8_t bit, bool active_low);

/* ---- User-attended diagnostics (Kconfig-gated, MAINS DISCONNECTED) ---- */

/* Polarity/mapping probe against the confirmed expander: Phase 1 all-relay
 * ON then OFF with record-the-answer prompts, Phase 2 a per-bit walk of all
 * eight outputs. Aborts unless the backend is active. */
void relay_expander_map_test(void);

/* Write+readback discriminator: walks the baseline then each single-relay bit,
 * logging wrote/read pairs so a pin that does not obey writes is named. */
void relay_expander_pcf_test(void);

/* Controlled ON-path walk of the two dosing relays: OFF/OFF -> R1 ON -> OFF ->
 * R2 ON -> OFF -> BOTH ON -> OFF, printing per step the expected byte, the byte
 * written, the esp_err and a live readback, then a self-judging summary table.
 * Kconfig-gated, MAINS DISCONNECTED, and deliberately NOT subject to the
 * actuation gate - proving the pins obey is what verification is for. */
void relay_expander_act_test(void);

#endif /* RELAY_EXPANDER_H */