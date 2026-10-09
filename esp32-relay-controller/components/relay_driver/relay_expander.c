#include "relay_expander.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

/* Kconfig supplies the bus pins in the firmware build. The fallbacks keep this
 * file compilable in projects without Kconfig (the QEMU build links it and
 * unit-tests the pure helpers; init tolerates the emulated bus with no slaves
 * and reports the backend inactive). The confirmed board uses GPIO22/GPIO23 —
 * unchanged from the existing project, verified in main/Kconfig.projbuild. */
#ifndef CONFIG_WEIGHT_DEMO_I2C_SDA
#define CONFIG_WEIGHT_DEMO_I2C_SDA 22
#endif
#ifndef CONFIG_WEIGHT_DEMO_I2C_SCL
#define CONFIG_WEIGHT_DEMO_I2C_SCL 23
#endif
/* Polarity default: ACTIVE-HIGH — MEASURED on the physical board, which turned
 * out to invert the earlier derivation. See the ACTIVE LEVEL section of
 * relay_expander.h for the field evidence and why it supersedes it.
 *
 * The fallback below (no Kconfig, i.e. the QEMU test project) deliberately
 * matches the SHIPPED default so the suite exercises the polarity the firmware
 * actually ships. Getting this backwards would let the tests keep passing
 * while asserting the wrong wire levels. */
#if defined(CONFIG_WEIGHT_DEMO_RELAY_POLARITY_ACTIVE_LOW)
#define EXPANDER_ACTIVE_LOW 1
#else
#define EXPANDER_ACTIVE_LOW 0
#endif
/* ON commands are gated on field verification. OFF is never gated. */
#ifndef CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED
#define CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED 0
#endif

static const char *TAG = "relay_expander";

/*
 * The expander could not be reached, so nothing can be written to it.
 *
 * On an ACTIVE-HIGH board this is NOT a benign "fall back to GPIO" condition:
 * the PCF8574 powers up with every port pulled HIGH, which for this polarity is
 * the ENERGIZED level, and with no ACK there is no way to clear it from
 * firmware. The relays may therefore be ON right now and stay ON until the bus
 * is fixed. That is a property of the hardware, not something the firmware can
 * undo — but it must never be left unsaid on the bench.
 */
static void report_expander_unreachable(const char *why)
{
    if (EXPANDER_ACTIVE_LOW) {
        /* Active-LOW: the power-up level is the INACTIVE one, so the board
         * rests safe and this stays an ordinary error. */
        ESP_LOGE(TAG, "relay expander 0x%02x unreachable (%s) - I2C relay output "
                      "unavailable", (unsigned)RELAY_EXPANDER_ADDR, why);
        return;
    }

    ESP_LOGE(TAG, "relay expander 0x%02x unreachable (%s).",
             (unsigned)RELAY_EXPANDER_ADDR, why);
    ESP_LOGE(TAG, "*** ACTIVE-HIGH BOARD WITH NO RELAY BUS: the PCF8574 powers up "
                  "with every port HIGH, which is the ENERGIZED level, and with no "
                  "ACK there is NO WAY to drive it OFF from firmware. ***");
    ESP_LOGE(TAG, "*** THE RELAYS MAY BE ENERGIZED NOW AND MAY STAY ENERGIZED. "
                  "DISCONNECT AC LOADS AND FIX THE I2C BUS BEFORE PROCEEDING. ***");
}

#define I2C_BUS_FREQ_HZ   100000U  /* standard mode first */
#define PROBE_TIMEOUT_MS  20
#define XFER_TIMEOUT_MS   100
#define SCAN_ADDR_FIRST   0x08U
#define SCAN_ADDR_LAST    0x77U

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_detected;
static bool s_chip_unsupported;   /* register-type device at 0x22 */
static bool s_backend_active;
static bool s_init_done;          /* one discovery attempt per boot */
static uint8_t s_shadow;
/* "ack" must mean a real write was ACKed — never a power-on default. */
static bool s_last_write_ok;
/* H1: set by ANY failed write (NACK, timeout, bus error). A write that was
 * reported failed to the master may still have been latched by the device, so
 * s_shadow no longer proves the wire state. While set, the transition-only
 * short-circuit is disabled and the next OFF is always actually written. Cleared
 * only by a write that ACKed. */
static bool s_shadow_unknown;

#ifdef BITS_QEMU_TEST_HOOKS
/* TEST-ONLY bus seam (tests/qemu/CMakeLists.txt defines the macro; the
 * production project does not, so none of this exists in the firmware image). */
static esp_err_t (*s_test_tx)(uint8_t);
static esp_err_t (*s_test_rx)(uint8_t *);
static int s_test_gate = -1;  /* -1 = real Kconfig gate, 0/1 = forced */
#endif

/* ------------------------------------------------------------------------
 * Pure helpers — no bus, no state; unit-tested in the QEMU suite.
 * ------------------------------------------------------------------------ */

bool relay_expander_bit_for_index(uint8_t index, uint8_t *bit)
{
    if (bit == NULL) return false;

    /* Relay 1 = P6, Relay 2 = P7. Only these two outputs are confirmed; any
     * other index is NOT on this expander and must not be written here. */
    if (index == 0U) { *bit = RELAY_EXPANDER_BIT_RELAY1; return true; }
    if (index == 1U) { *bit = RELAY_EXPANDER_BIT_RELAY2; return true; }
    return false;
}

uint8_t relay_expander_shadow_set(uint8_t shadow, uint8_t bit, bool on,
                                  bool active_low)
{
    uint8_t mask = (uint8_t)(1U << bit);
    bool wire_high = active_low ? !on : on;
    return wire_high ? (uint8_t)(shadow | mask)
                     : (uint8_t)(shadow & (uint8_t)~mask);
}

uint8_t relay_expander_shadow_all(uint8_t shadow, bool on, bool active_low)
{
    bool wire_high = active_low ? !on : on;
    return wire_high ? (uint8_t)(shadow | RELAY_EXPANDER_USED_MASK)
                     : (uint8_t)(shadow & (uint8_t)~RELAY_EXPANDER_USED_MASK);
}

bool relay_expander_bit_is_on(uint8_t shadow, uint8_t bit, bool active_low)
{
    bool wire_high = (shadow & (uint8_t)(1U << bit)) != 0U;
    return active_low ? !wire_high : wire_high;
}

/* ------------------------------------------------------------------------
 * Bus internals
 * ------------------------------------------------------------------------ */

/* The one physical write path. Every byte that reaches the bus is logged here
 * with its result — the field-diagnostic contract. */
static esp_err_t bus_tx(uint8_t value)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_tx != NULL) return s_test_tx(value);
#endif
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(s_dev, &value, 1U, XFER_TIMEOUT_MS);
}

/* Plain one-byte read: a PCF8574 returns the LIVE pin levels. */
static esp_err_t bus_rx(uint8_t *value)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_rx != NULL) return s_test_rx(value);
#endif
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_receive(s_dev, value, 1U, XFER_TIMEOUT_MS);
}

static esp_err_t expander_write(uint8_t value)
{
    esp_err_t err = bus_tx(value);
    ESP_LOGI(TAG, "I2C relay write addr=0x%02x value=0x%02x (%s)",
             (unsigned)RELAY_EXPANDER_ADDR, (unsigned)value, esp_err_to_name(err));
    if (err == ESP_OK) {
        s_shadow = value;
        s_last_write_ok = true;
        s_shadow_unknown = false;
    } else {
        s_last_write_ok = false;
        /* The device may have latched this byte even though the master saw a
         * failure: the shadow is no longer trustworthy until a write ACKs. */
        s_shadow_unknown = true;
        ESP_LOGE(TAG, "expander 0x%02x write FAILED: %s - shadow not updated, "
                      "state UNKNOWN (next OFF will be written)",
                 (unsigned)RELAY_EXPANDER_ADDR, esp_err_to_name(err));
    }
    return err;
}

/* OFF pattern written to the bus whatever the shadow says (H1). Used by the boot
 * write and the force path; the six unused bits ride along unchanged. */
static esp_err_t force_off_write(void)
{
    return expander_write(relay_expander_shadow_all(s_shadow, false, EXPANDER_ACTIVE_LOW));
}

/* ------------------------------------------------------------------------
 * Discovery + chip gate + EARLY OFF write
 * ------------------------------------------------------------------------ */

esp_err_t relay_expander_init(void)
{
    if (s_init_done) {
        return s_backend_active ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_init_done = true;

    /* Seed the shadow at the INACTIVE pattern before any bus work. The
     * PCF8574's power-up state is all ports HIGH; under the measured ACTIVE-HIGH
     * polarity that is "both relays off", so this is what a device we never
     * reached should read as. Leaving it at 0 would make an unseeded shadow
     * report BOTH relays ENERGIZED (active-LOW: a clear bit is energized),
     * which is exactly the wrong thing for a diagnostic dump to claim. */
    s_shadow = relay_expander_shadow_all(0x00U, false, EXPANDER_ACTIVE_LOW);

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)CONFIG_WEIGHT_DEMO_I2C_SDA,
        .scl_io_num = (gpio_num_t)CONFIG_WEIGHT_DEMO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        /* Tolerated, never fatal: QEMU (controller emulated, no slaves) and
         * wiring faults land here. Boot continues on the GPIO fallback. */
        ESP_LOGE(TAG, "I2C bus create failed: %s - expander discovery skipped",
                 esp_err_to_name(err));
        s_bus = NULL;
        report_expander_unreachable("I2C bus create failed");
        return err;
    }
    ESP_LOGI(TAG, "I2C bus up: SDA=GPIO%d SCL=GPIO%d @ %u kHz",
             CONFIG_WEIGHT_DEMO_I2C_SDA, CONFIG_WEIGHT_DEMO_I2C_SCL,
             (unsigned)(I2C_BUS_FREQ_HZ / 1000U));

    /* Full sweep, log-only except for the confirmed address: an inventory of
     * what else shares the bus is the evidence a future board revision needs. */
    for (uint16_t addr = SCAN_ADDR_FIRST; addr <= SCAN_ADDR_LAST; addr++) {
        if (i2c_master_probe(s_bus, addr, PROBE_TIMEOUT_MS) != ESP_OK) continue;
        ESP_LOGI(TAG, "I2C scan: device ACK at 0x%02x", (unsigned)addr);
        if (addr >= 0x38U && addr <= 0x3FU) {
            ESP_LOGI(TAG, "note: 0x%02x is in the PCF8574A address range "
                          "(possible A-variant)", (unsigned)addr);
        }
    }

    if (i2c_master_probe(s_bus, RELAY_EXPANDER_ADDR, PROBE_TIMEOUT_MS) != ESP_OK) {
        report_expander_unreachable("PCF8574T did not ACK at 0x22, A2=0 A1=1 A0=0");
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RELAY_EXPANDER_ADDR,
        .scl_speed_hz = I2C_BUS_FREQ_HZ,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        report_expander_unreachable("device attach failed");
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    s_detected = true;
    ESP_LOGI(TAG, "relay expander PCF8574T detected at 0x%02x | Relay1=P6(0x%02x pin 11) "
                  "Relay2=P7(0x%02x pin 12)",
             (unsigned)RELAY_EXPANDER_ADDR,
             (unsigned)RELAY_EXPANDER_MASK_RELAY1,
             (unsigned)RELAY_EXPANDER_MASK_RELAY2);

    /* Chip-type gate, READ-ONLY: a PCF8574 answers a plain 2-byte read with
     * its pin state twice (no registers, no auto-increment) -> b0 == b1.
     * b0 != b1 means a register device (MCP23017/PCA9555 default-pointer
     * reads differ) and PCF8574-style byte writes to it would be chaotic:
     * disable the backend rather than write. A freshly powered MCP23017 can
     * read 0xFF/0xFF and pass — which is why ON commands additionally require
     * WEIGHT_DEMO_POLARITY_VERIFIED. */
    uint8_t raw[2] = {0xAAU, 0xAAU};
    err = i2c_master_receive(s_dev, raw, sizeof(raw), XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander 0x%02x raw read failed: %s - backend disabled",
                 (unsigned)RELAY_EXPANDER_ADDR, esp_err_to_name(err));
        s_chip_unsupported = true;
        report_expander_unreachable("chip-gate read failed");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "expander 0x%02x raw read: b0=0x%02x b1=0x%02x",
             (unsigned)RELAY_EXPANDER_ADDR, raw[0], raw[1]);
    if (raw[0] != raw[1]) {
        ESP_LOGE(TAG, "register-type I2C expander detected at 0x%02x - "
                      "PCF8574 driver disabled; report chip for "
                      "MCP23017/PCA9555 support",
                 (unsigned)RELAY_EXPANDER_ADDR);
        s_chip_unsupported = true;
        return ESP_ERR_NOT_FOUND;
    }

    /* Shadow seeded from the LIVE pin state — never blind. The six unused bits
     * ride along and are preserved by every write. */
    s_shadow = raw[0];
    s_backend_active = true;

    /* EARLY OFF WRITE — the fail-safe, issued here so both relays are in the
     * inactive state before Wi-Fi, the WebSocket client or the supervisor
     * exist. relay_driver calls this from relay_init_all(), which app_main runs
     * first, so the ordering is: pre-drive + expander OFF write, THEN radio,
     * web API and supervisor.
     *
     * Under the MEASURED active-HIGH polarity this write is not a formality —
     * it is precisely what DE-ENERGIZES the power-up state. The PCF8574 comes
     * up with every port pulled HIGH, which on this board is the ENERGIZED
     * level, so between power-up and this write the relays are ON. Firmware
     * cannot shrink that window to zero; it can only close it as early as
     * possible, which is what this ordering does. A hardware mitigation
     * (pull-downs on the relay-drive inputs, or a board revision that
     * inverts the stage) is the only way to remove it. */
    err = force_off_write();  /* H1: unconditional, never skipped by the shadow */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot fail-safe write to 0x%02x failed - backend disabled",
                 (unsigned)RELAY_EXPANDER_ADDR);
        s_backend_active = false;
        return err;
    }

    /* One boot block that answers the only question an operator has when the
     * valves do not move: which backend is live, and is ON permitted? Every
     * input to that decision is printed, including the COMPILED gate value, so
     * the answer is never inferred from behaviour. */
    ESP_LOGI(TAG, "--- relay actuation state ---");
    ESP_LOGI(TAG, "  expander detected      : %s", s_detected ? "yes" : "NO");
    ESP_LOGI(TAG, "  backend active         : %s", s_backend_active ? "yes" : "NO");
    ESP_LOGI(TAG, "  configured polarity    : %s",
             EXPANDER_ACTIVE_LOW ? "ACTIVE-LOW (0 = energized)"
                                 : "ACTIVE-HIGH (1 = energized)");
    ESP_LOGI(TAG, "  WEIGHT_DEMO_POLARITY_VERIFIED (compiled) : %d",
             (int)CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED);
    ESP_LOGI(TAG, "  ON commands allowed    : %s",
             relay_expander_actuation_allowed() ? "yes" : "NO (suppressed)");
    ESP_LOGI(TAG, "  OFF commands           : always allowed (never gated)");
    ESP_LOGI(TAG, "  shadow byte now        : 0x%02x (relay bits %s)",
             (unsigned)s_shadow,
             (s_shadow & RELAY_EXPANDER_USED_MASK) ? "SET" : "CLEARED");

    if (!relay_expander_actuation_allowed()) {
        ESP_LOGW(TAG, "ON commands suppressed until WEIGHT_DEMO_POLARITY_VERIFIED=y "
                      "(run MAPTEST/PCF_TEST to confirm polarity physically); "
                      "OFF is always driven, so the relays stay de-energized");
    }

    return ESP_OK;
}

bool relay_expander_backend_active(void) { return s_backend_active; }
bool relay_expander_detected(void)       { return s_detected; }
bool relay_expander_active_low(void)     { return EXPANDER_ACTIVE_LOW != 0; }

bool relay_expander_actuation_allowed(void)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_gate >= 0) return s_backend_active && (s_test_gate != 0);
#endif
    return s_backend_active && (CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED != 0);
}

uint8_t relay_expander_shadow(void) { return s_shadow; }

/* ------------------------------------------------------------------------
 * Actuation — transition-only; caller (relay_driver) serializes.
 * ------------------------------------------------------------------------ */

esp_err_t relay_expander_set_channel(uint8_t index, bool on)
{
    /* Every command for a dosing channel is traced: what was asked, whether it
     * was accepted, and why not when it was not. A rejected ON that leaves the
     * reported state looking OPEN is the exact failure this trace exists to
     * make impossible to miss. */
    const char *want = on ? "ON" : "OFF";

    if (!s_backend_active) {
        ESP_LOGW(TAG, "REL %s idx%u REJECTED | backend inactive (no expander at 0x%02x)",
                 want, (unsigned)index, (unsigned)RELAY_EXPANDER_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t bit = 0U;
    if (!relay_expander_bit_for_index(index, &bit)) {
        /* Not a confirmed expander output. Refusing is the safe answer: a
         * guessed bit could drive an unrelated load sharing P0..P5. */
        ESP_LOGW(TAG, "REL %s idx%u REJECTED | index is not on the expander "
                      "(only 0 and 1 are)", want, (unsigned)index);
        return ESP_ERR_INVALID_ARG;
    }

    if (on && !relay_expander_actuation_allowed()) {
        ESP_LOGW(TAG, "REL ON idx%u (bit%u) REJECTED | WEIGHT_DEMO_POLARITY_VERIFIED=%d "
                      "- zero bus traffic, shadow stays 0x%02x",
                 (unsigned)index, (unsigned)bit,
                 (int)CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED, (unsigned)s_shadow);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t shadow_before = s_shadow;
    uint8_t next = relay_expander_shadow_set(s_shadow, bit, on, EXPANDER_ACTIVE_LOW);

    if (next == s_shadow && !s_shadow_unknown) {
        /* Steady state (state KNOWN): no bus traffic. The only line
         * rate-limited, and never a transition or a rejection. After a failed
         * write the state is UNKNOWN and this is skipped, so OFF is written. */
        ESP_LOGD(TAG, "REL %s idx%u ACCEPTED (no change) | shadow 0x%02x",
                 want, (unsigned)index, (unsigned)shadow_before);
        return ESP_OK;
    }

    esp_err_t err = expander_write(next);
    ESP_LOGI(TAG, "REL %s idx%u (bit%u) %s | addr=0x%02x shadow 0x%02x -> 0x%02x "
                  "| write=0x%02x | %s",
             want, (unsigned)index, (unsigned)bit,
             (err == ESP_OK) ? "ACCEPTED" : "REJECTED (bus error)",
             (unsigned)RELAY_EXPANDER_ADDR, (unsigned)shadow_before, (unsigned)next,
             (unsigned)next, esp_err_to_name(err));
    return err;
}

esp_err_t relay_expander_all_off(void)
{
    if (!s_backend_active) return ESP_ERR_NOT_FOUND;

    /* Deliberately NOT gated on verification — see the header. OFF is the safe
     * direction in every reading of the polarity, so every fault path can
     * issue it unconditionally. */
    /* ACTIVE-HIGH: OFF CLEARS the relay bits. */
    uint8_t next = relay_expander_shadow_all(s_shadow, false, EXPANDER_ACTIVE_LOW);
    /* Silent only while the state is KNOWN; UNKNOWN (a failed write) always writes. */
    if (next == s_shadow && !s_shadow_unknown) return ESP_OK;
    return expander_write(next);
}

esp_err_t relay_expander_force_all_off(void)
{
    if (!s_backend_active) return ESP_ERR_NOT_FOUND;

    /* Unconditional: the shadow and the unknown flag are ignored on purpose. */
    esp_err_t err = force_off_write();
    if (err != ESP_OK) return err;

    /* Read-back. A PCF8574 read returns the LIVE pin level, not the latch. With
     * the shipped ACTIVE-HIGH polarity OFF is pin LOW, which the chip drives
     * strongly, so a relay bit that reads low proves the latch is clear (or the
     * pin is held low externally): not energized. Under ACTIVE-LOW, OFF is pin
     * HIGH (weak pull-up), so a high read only shows nothing pulls it low; it
     * cannot prove the latch. Either way a read that fails or disagrees on the
     * relay bits is NOT a confirmed OFF: fail-safe, mark UNKNOWN and report
     * an error so the caller keeps retrying. */
    uint8_t rb = 0xEEU;
    esp_err_t rerr = bus_rx(&rb);
    if (rerr != ESP_OK) {
        s_shadow_unknown = true;
        ESP_LOGE(TAG, "expander 0x%02x OFF read-back FAILED: %s - OFF not confirmed",
                 (unsigned)RELAY_EXPANDER_ADDR, esp_err_to_name(rerr));
        return rerr;
    }
    if ((rb & RELAY_EXPANDER_USED_MASK) != (s_shadow & RELAY_EXPANDER_USED_MASK)) {
        s_shadow_unknown = true;
        ESP_LOGE(TAG, "expander 0x%02x OFF read-back mismatch: wrote 0x%02x read 0x%02x "
                      "- OFF not confirmed", (unsigned)RELAY_EXPANDER_ADDR,
                 (unsigned)s_shadow, (unsigned)rb);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

bool relay_expander_state_unknown(void) { return s_shadow_unknown; }

#ifdef BITS_QEMU_TEST_HOOKS
/* TEST-ONLY: attach a fake bus and mark the backend active so relay_driver and
 * the controller run their real code against a modelled device. Passing NULLs
 * detaches and returns the module to "no backend". */
void relay_expander_test_set_bus(esp_err_t (*tx)(uint8_t), esp_err_t (*rx)(uint8_t *),
                                 uint8_t initial_shadow, int gate)
{
    s_test_gate = (tx != NULL) ? gate : -1;
    s_test_tx = tx;
    s_test_rx = rx;
    s_init_done = true;
    s_detected = (tx != NULL);
    s_backend_active = (tx != NULL);
    s_shadow = initial_shadow;
    s_shadow_unknown = false;
    s_last_write_ok = true;
}
#endif

esp_err_t relay_expander_all_on(void)
{
    if (!s_backend_active) return ESP_ERR_NOT_FOUND;
    if (!relay_expander_actuation_allowed()) return ESP_ERR_INVALID_STATE;

    uint8_t next = relay_expander_shadow_all(s_shadow, true, EXPANDER_ACTIVE_LOW);
    if (next == s_shadow) return ESP_OK;
    return expander_write(next);
}

bool relay_expander_channel_on(uint8_t index)
{
    uint8_t bit = 0U;
    if (!relay_expander_bit_for_index(index, &bit)) return false;
    return relay_expander_bit_is_on(s_shadow, bit, EXPANDER_ACTIVE_LOW);
}

bool relay_expander_last_write_ok(uint8_t index)
{
    uint8_t bit = 0U;
    if (!relay_expander_bit_for_index(index, &bit)) return false;
    return s_last_write_ok;
}


/* ------------------------------------------------------------------------
 * ACT_TEST — controlled ON-path diagnostic (Kconfig-gated, MAINS DISCONNECTED)
 *
 * Walks the two dosing relays through every combination and prints, per step,
 * the intended wire byte, the byte actually written, the esp_err, and a plain
 * READBACK of the live pin state (a PCF8574 read returns the pins, there being
 * no register pointer). It ends with an expectation-vs-actual table so the run
 * judges itself rather than asking the operator to eyeball five log lines.
 *
 * Deliberately writes through expander_write() directly rather than through
 * relay_expander_set_channel(), so it works regardless of the actuation gate:
 * proving the pins obey is exactly what verification requires, and this is the
 * same exemption PCF_TEST and MAPTEST already take.
 * ------------------------------------------------------------------------ */

/* Expected wire byte for a requested (coarse, fine) pair, computed with the
 * module's own helpers under the CONFIGURED polarity, so the expectation cannot
 * disagree with the code that does the real work. Unused bits are held at
 * whatever the live read showed. */
static uint8_t act_expected(bool coarse, bool fine, uint8_t unused_bits)
{
    uint8_t base = relay_expander_shadow_all(unused_bits, false, EXPANDER_ACTIVE_LOW);
    base = relay_expander_shadow_set(base, RELAY_EXPANDER_BIT_RELAY1,
                                     coarse, EXPANDER_ACTIVE_LOW);
    base = relay_expander_shadow_set(base, RELAY_EXPANDER_BIT_RELAY2,
                                     fine, EXPANDER_ACTIVE_LOW);
    return base;
}

static void act_step(const char *label, bool coarse, bool fine,
                     uint8_t unused_bits, uint8_t *row_expected,
                     uint8_t *row_wrote, uint8_t *row_read, bool *row_ok)
{
    uint8_t want = act_expected(coarse, fine, unused_bits);

    ESP_LOGW(TAG, "ACT_TEST: %-22s -> driving relay1=%s relay2=%s | expecting 0x%02x",
             label, coarse ? "ON" : "off", fine ? "ON" : "off", (unsigned)want);

    esp_err_t err = expander_write(want);

    uint8_t readback = 0xEEU;
    bool read_ok = (i2c_master_receive(s_dev, &readback, 1U, XFER_TIMEOUT_MS) == ESP_OK);
    if (!read_ok) readback = 0xEEU;

    bool ok = (err == ESP_OK) && read_ok && (readback == want);

    ESP_LOGW(TAG, "ACT_TEST:   wrote=0x%02x (%s)  readback=0x%02x (%s)  %s",
             (unsigned)want, esp_err_to_name(err),
             (unsigned)readback,
             read_ok ? "live pins" : "READ FAILED",
             ok ? "MATCH" : "*** MISMATCH ***");
    if (read_ok && readback != want) {
        ESP_LOGE(TAG, "ACT_TEST: pin did not follow the write (wrote 0x%02x, read "
                      "0x%02x) - externally driven, wrong device, or not a PCF8574",
                 (unsigned)want, (unsigned)readback);
    }

    *row_expected = want;
    *row_wrote = want;
    *row_read = readback;
    *row_ok = ok;

    /* A step is only meaningful if the operator looks at the board during it. */
    vTaskDelay(pdMS_TO_TICKS(2500));
}

void relay_expander_act_test(void)
{
    if (!s_backend_active || s_dev == NULL) {
        ESP_LOGE(TAG, "ACT_TEST aborted - expander backend not active "
                      "(detection or chip gate failed at 0x%02x)",
                 (unsigned)RELAY_EXPANDER_ADDR);
        return;
    }

    for (int i = 5; i >= 1; i--) {
        ESP_LOGW(TAG, "ACT_TEST starts in %d s - MAINS/LOAD MUST BE DISCONNECTED", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Hold the unused bits at whatever the live read showed, then start from a
     * known OFF/OFF so the walk is reproducible. */
    uint8_t raw = 0xFFU;
    if (i2c_master_receive(s_dev, &raw, 1U, XFER_TIMEOUT_MS) != ESP_OK) raw = 0xFFU;
    const uint8_t unused_bits = (uint8_t)(raw & (uint8_t)~RELAY_EXPANDER_USED_MASK);

    ESP_LOGW(TAG, "ACT_TEST: expander 0x%02x, polarity %s, unused bits held at 0x%02x",
             (unsigned)RELAY_EXPANDER_ADDR,
             EXPANDER_ACTIVE_LOW ? "ACTIVE-LOW" : "ACTIVE-HIGH",
             (unsigned)unused_bits);
    ESP_LOGW(TAG, "ACT_TEST: with P0..P5 reading HIGH the expected bytes are "
                  "0x3f (off/off), 0x7f (relay1), 0xbf (relay2), 0xff (both), 0x3f (off/off)");

    uint8_t exp[5], wrote[5], rd[5];
    bool ok[5];

    act_step("both OFF (start)",  false, false, unused_bits, &exp[0], &wrote[0], &rd[0], &ok[0]);
    act_step("RELAY 1 ON",         true,  false, unused_bits, &exp[1], &wrote[1], &rd[1], &ok[1]);
    act_step("RELAY 1 OFF",        false, false, unused_bits, &exp[2], &wrote[2], &rd[2], &ok[2]);
    act_step("RELAY 2 ON",         false, true,  unused_bits, &exp[3], &wrote[3], &rd[3], &ok[3]);
    act_step("RELAY 2 OFF",        false, false, unused_bits, &exp[4], &wrote[4], &rd[4], &ok[4]);

    /* Final state: both OFF, written once more so the run always ends safe. */
    (void)expander_write(act_expected(false, false, unused_bits));

    int failures = 0;
    for (int i = 0; i < 5; i++) if (!ok[i]) failures++;

    ESP_LOGW(TAG, "ACT_TEST summary (expectation vs actual):");
    ESP_LOGW(TAG, "  step                  expected  wrote  readback  verdict");
    static const char *names[5] = { "both OFF (start)", "RELAY 1 ON", "RELAY 1 OFF",
                                    "RELAY 2 ON", "RELAY 2 OFF" };
    for (int i = 0; i < 5; i++) {
        ESP_LOGW(TAG, "  %-20s  0x%02x    0x%02x   0x%02x     %s",
                 names[i], (unsigned)exp[i], (unsigned)wrote[i], (unsigned)rd[i],
                 ok[i] ? "MATCH" : "MISMATCH");
    }
    ESP_LOGW(TAG, "ACT_TEST complete: %d/5 steps matched%s", 5 - failures,
             failures ? " - SEE MISMATCHES ABOVE" : "");
    ESP_LOGW(TAG, "ACT_TEST: did relay 1 physically click on the RELAY 1 ON step "
                  "and relay 2 on the RELAY 2 ON step? If the bytes matched but "
                  "nothing clicked, the expander is not driving these relays.");
}

/* ------------------------------------------------------------------------
 * User-attended diagnostics (Kconfig-gated, MAINS DISCONNECTED).
 * The ONLY places that write raw all-bits patterns.
 * ------------------------------------------------------------------------ */

void relay_expander_map_test(void)
{
    if (!s_backend_active) {
        ESP_LOGE(TAG, "MAPTEST aborted - expander backend not active "
                      "(detection or chip gate failed; raw writes to an "
                      "unknown register device are unsafe)");
        return;
    }

    for (int i = 5; i >= 1; i--) {
        ESP_LOGW(TAG, "MAPTEST starts in %d s - MAINS MUST BE DISCONNECTED", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Phase 1 — polarity probe on the RELAY bits only. P0..P5 are left at
     * whatever the live read showed: they are not relay outputs and poking
     * them could disturb an unrelated load sharing the expander. */
    const uint8_t off_raw = relay_expander_shadow_all(s_shadow, false, EXPANDER_ACTIVE_LOW);
    const uint8_t on_raw  = relay_expander_shadow_all(s_shadow, true, EXPANDER_ACTIVE_LOW);

    ESP_LOGW(TAG, "MAPTEST Phase 1: driving BOTH relays to the ACTIVE level "
                  "(value=0x%02x), hold 3 s", (unsigned)on_raw);
    (void)expander_write(on_raw);
    ESP_LOGW(TAG, ">>> did BOTH relays energize? "
                  "=> ACTIVE_%s if YES - record the answer",
             EXPANDER_ACTIVE_LOW ? "LOW" : "HIGH");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGW(TAG, "MAPTEST Phase 1: driving BOTH relays to the INACTIVE level "
                  "(value=0x%02x), hold 3 s", (unsigned)off_raw);
    (void)expander_write(off_raw);
    ESP_LOGW(TAG, ">>> did BOTH relays de-energize? => confirms the configured "
                  "polarity");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Phase 2 — per-bit walk over all eight outputs, one at a time, baseline
     * restored between. This is the table that confirms (or corrects) the
     * P6/P7 assignment. */
    ESP_LOGI(TAG, "MAPTEST Phase 2: per-bit walk from baseline 0x%02x",
             (unsigned)off_raw);
    for (int bit = 7; bit >= 0; bit--) {
        uint8_t value = relay_expander_shadow_set(off_raw, (uint8_t)bit, true,
                                                  EXPANDER_ACTIVE_LOW);
        (void)expander_write(value);
        ESP_LOGW(TAG, "MAPTEST addr=0x%02x bit=%d value=0x%02x "
                      "-> which physical relay responded? (record, 2.5 s)",
                 (unsigned)RELAY_EXPANDER_ADDR, bit, (unsigned)value);
        vTaskDelay(pdMS_TO_TICKS(2500));
        (void)expander_write(off_raw);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "MAPTEST complete - expander at inactive 0x%02x. Expected: "
                  "bit6 (P6, pin 11) = Relay 1 / coarse and bit7 (P7, pin 12) "
                  "= Relay 2 / fine. Record any difference before enabling AC "
                  "loads.", (unsigned)off_raw);
}

/* Write through the single logged expander_write() path (write-log contract
 * holds), then immediately plain-read the device (PCF8574 reads return the
 * LIVE pin state; no register pointer exists) and log the pair. */
static bool expander_write_readback(uint8_t value)
{
    (void)expander_write(value);

    uint8_t readback = 0xEEU;
    esp_err_t err = i2c_master_receive(s_dev, &readback, 1U, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCF_TEST: addr=0x%02x wrote=0x%02x readback FAILED (%s)",
                 (unsigned)RELAY_EXPANDER_ADDR, (unsigned)value, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "PCF_TEST: addr=0x%02x wrote=0x%02x read=0x%02x (%s)",
             (unsigned)RELAY_EXPANDER_ADDR, (unsigned)value, readback,
             (readback == value) ? "pin obeys write" : "MISMATCH");
    if (readback != value) {
        ESP_LOGW(TAG, "PCF_TEST MISMATCH addr=0x%02x wrote=0x%02x read=0x%02x "
                      "- pin not obeying write (externally driven? input expander?)",
                 (unsigned)RELAY_EXPANDER_ADDR, (unsigned)value, (unsigned)readback);
        return false;
    }
    s_shadow = readback;
    return true;
}

void relay_expander_pcf_test(void)
{
    if (!s_backend_active) {
        ESP_LOGE(TAG, "PCF_TEST aborted - expander backend not active "
                      "(detection or chip gate failed)");
        return;
    }

    for (int i = 5; i >= 1; i--) {
        ESP_LOGW(TAG, "PCF_TEST starts in %d s - MAINS MUST BE DISCONNECTED", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const uint8_t off_raw = relay_expander_shadow_all(s_shadow, false, EXPANDER_ACTIVE_LOW);

    ESP_LOGW(TAG, "PCF_TEST: baseline (both relays at the INACTIVE level) "
                  "value=0x%02x - hold 2.5 s", (unsigned)off_raw);
    (void)expander_write_readback(off_raw);
    vTaskDelay(pdMS_TO_TICKS(2500));

    for (int bit = 7; bit >= 0; bit--) {
        uint8_t value = relay_expander_shadow_set(off_raw, (uint8_t)bit, true,
                                                  EXPANDER_ACTIVE_LOW);
        (void)expander_write_readback(value);
        ESP_LOGW(TAG, "PCF_TEST: bit %d (0x%02x)%s -> which physical relay "
                      "responded? (record; 2.5 s)",
                 bit, (unsigned)(1U << bit),
                 (bit == (int)RELAY_EXPANDER_BIT_RELAY1) ? " [expected Relay 1 / coarse]" :
                 (bit == (int)RELAY_EXPANDER_BIT_RELAY2) ? " [expected Relay 2 / fine]" : "");
        vTaskDelay(pdMS_TO_TICKS(2500));
        (void)expander_write(off_raw);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    (void)expander_write(off_raw);

    ESP_LOGW(TAG, "PCF_TEST summary - interpret your notes:");
    ESP_LOGW(TAG, "  relay energized on the ACTIVE-level steps => configured "
                  "polarity is correct");
    ESP_LOGW(TAG, "  never energized AND all readbacks matched => the device "
                  "accepts writes but drives no relay - escalate with the scan list");
    ESP_LOGW(TAG, "  readback MISMATCHes => pins externally driven - same escalation");
    ESP_LOGI(TAG, "PCF_TEST complete - expander left at inactive 0x%02x",
             (unsigned)off_raw);
}