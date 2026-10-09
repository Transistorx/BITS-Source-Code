#include "relay_driver.h"
#include "relay_expander.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <stdio.h>

/* Kconfig gate for the "not field-verified" boot warning; fallback keeps
 * non-Kconfig builds (QEMU) compiling with the warning ON (safe default). */
#ifndef CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED
#define CONFIG_WEIGHT_DEMO_POLARITY_VERIFIED 0
#endif
/* C5-4: AO pre-drive defaults ON in every build (safety measure — see
 * relay_init_all). */
#ifndef CONFIG_WEIGHT_DEMO_AO_PINS_PREDRIVE
#define CONFIG_WEIGHT_DEMO_AO_PINS_PREDRIVE 1
#endif

static const char *TAG = "relay_driver";

/* Reference-schematic AO map: index 0..11 = AO1..AO12. Used by the direct-GPIO
 * fallback backend and by the board variants that drive the AO nets directly.
 * On THIS board indices 0 and 1 (Relay 1 / Relay 2) are actuated through the
 * confirmed PCF8574T at 0x22 and are never driven as GPIOs while that backend
 * is active — see relay_set(). */
static const gpio_num_t s_relay_gpio[RELAY_COUNT] = {
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_25,
    GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_32, GPIO_NUM_33,
    GPIO_NUM_4,  GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_2,
};

static bool s_state[RELAY_COUNT];
static bool s_initialized;

/* True when Relay 1 and Relay 2 are served by the confirmed I2C expander. */
static bool s_use_i2c;
static SemaphoreHandle_t s_i2c_mutex;
/* Suppression WARN latch: 0 = none, 1 = OFF-direction, 2 = ON-direction. */
static int s_suppress_logged;

/*
 * Serialises the cache check, the cache update and the pad write.
 *
 * Three different tasks call into this driver: the WebSocket client task, the
 * Wi-Fi event task, and the timeout supervisor. Without this lock those
 * sequences interleave, and the dangerous interleaving is: task A caches ON
 * and is preempted before writing the pad, task B (a failsafe) caches OFF and
 * drives the pad low, then task A resumes and drives the pad high. The cache
 * then reads OFF while the pad is ON, and because relay_set() skips a write
 * when the cache already matches, every later request to turn the channel off
 * short-circuits and the relay stays energized with nothing in the log.
 *
 * A spinlock rather than a mutex: the critical section is a handful of register
 * writes with no blocking call inside, so it cannot fail or time out, and it is
 * safe to enter from the event handlers. gpio_set_level() is a bare register
 * write with no internal lock, so it is safe in here. Logging stays outside.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

int relay_gpio_for_index(uint8_t index)
{
    if (index >= RELAY_COUNT) return -1;
    return (int)s_relay_gpio[index];
}

/* True when this index is actuated by the confirmed expander right now, and so
 * must NOT also be driven as a GPIO (the confirmed mapping is the expander —
 * remapping Relay 1/2 onto the AO nets is explicitly not wanted). */
static bool index_is_on_expander(uint8_t index)
{
    if (!s_use_i2c) return false;
    uint8_t bit = 0U;
    return relay_expander_bit_for_index(index, &bit);
}

esp_err_t relay_init_all(void)
{
#if CONFIG_WEIGHT_DEMO_AO_PINS_PREDRIVE
    uint64_t pin_mask = 0U;
    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        pin_mask |= 1ULL << (uint32_t)s_relay_gpio[i];
    }
#endif

    /* Pre-load the inactive level and clear the cached state before the pads
     * are switched to outputs, so no relay can glitch on during the change. */
    portENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        s_state[i] = false;
#if CONFIG_WEIGHT_DEMO_AO_PINS_PREDRIVE
        (void)gpio_set_level(s_relay_gpio[i], RELAY_INACTIVE_LEVEL);
#endif
    }
    portEXIT_CRITICAL(&s_lock);

#if CONFIG_WEIGHT_DEMO_AO_PINS_PREDRIVE
    /* gpio_config() may allocate, so it stays outside the critical section. */
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    ESP_LOGW(TAG, "AO pre-drive disabled by Kconfig - the direct-GPIO "
                  "fallback backend requires WEIGHT_DEMO_AO_PINS_PREDRIVE=y");
#endif

    portENTER_CRITICAL(&s_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "all %u relays initialised OFF", (unsigned)RELAY_COUNT);

    /* Confirmed relay path. relay_expander_init() is READ-ONLY discovery plus
     * the chip gate, and then the EARLY OFF WRITE — still before Wi-Fi or the
     * WebSocket client exist, preserving the "nothing can energize a relay
     * during init" guarantee. The GPIO pre-drive above is kept as well: it
     * holds the AO nets at a defined LOW through boot and keeps direct-GPIO
     * board variants working with zero code changes. */
    if (s_i2c_mutex == NULL) {
        s_i2c_mutex = xSemaphoreCreateMutex();
    }
    (void)relay_expander_init();
    s_use_i2c = relay_expander_backend_active() && (s_i2c_mutex != NULL);

    if (s_use_i2c) {
        ESP_LOGI(TAG, "relay backend: I2C expander PCF8574T 0x%02x (active-%s) | "
                      "Relay1=P6(coarse) Relay2=P7(fine)",
                 (unsigned)RELAY_EXPANDER_ADDR,
                 relay_expander_active_low() ? "LOW" : "HIGH");
        if (!relay_expander_actuation_allowed()) {
            ESP_LOGW(TAG, "ON commands suppressed - first run must be WITHOUT "
                          "AC loads until MAPTEST/PCF_TEST confirms polarity");
        }
    } else {
        ESP_LOGW(TAG, "relay backend: direct GPIO (expander 0x%02x unavailable) - "
                      "Relay 1/2 are NOT on the I2C path",
                 (unsigned)RELAY_EXPANDER_ADDR);
        /* The field report that drove the address correction also hints that
         * the relays which moved were driven by THIS path, and inverted — i.e.
         * the AO drive stage may be active-LOW, contradicting the reference
         * schematic. That is not established, so the level is NOT changed on a
         * guess; but a fallback that silently energizes the wrong way is
         * exactly what must not happen quietly. */
        ESP_LOGW(TAG, "*** dosing channels 1/2 are running on the DIRECT-GPIO "
                      "FALLBACK, whose polarity is UNVERIFIED on this board "
                      "(RELAY_ACTIVE_LEVEL=%d). ***", (int)RELAY_ACTIVE_LEVEL);
        ESP_LOGW(TAG, "*** Do not connect AC loads until this path has been "
                      "confirmed on the bench; fix the I2C bus to use the "
                      "expander path instead. ***");
        /* Channels 3..12 are unaffected by any of this: they are not on the
         * expander and keep the reference-schematic behaviour the LEVEL_GROUP
         * regression path depends on. */
    }
    return ESP_OK;
}

/* H1: update the commanded-state cache of the expander-served channels after a
 * group write (caller holds s_i2c_mutex). Success records `on`; a failure that
 * left the expander state UNKNOWN records ON (conservative), so a possibly
 * latched relay is never reported as OFF. */
static void expander_cache_follow(esp_err_t err, bool on)
{
    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        if (!index_is_on_expander(i)) continue;
        if (err == ESP_OK) s_state[i] = on;
        else if (relay_expander_state_unknown()) s_state[i] = true;
    }
}

/* One WARN per suppressed ON attempt, latched so a caller retrying the same
 * request cannot flood. Only ON is ever suppressed — OFF is deliberately
 * ungated (the inactive level is the expander's own power-up state, so a
 * de-energize write cannot energize anything), which means the 250 ms failsafe
 * re-assertion never reaches this function at all. */
static void relay_log_suppressed(void)
{
    if (s_suppress_logged != 1) {
        s_suppress_logged = 1;
        ESP_LOGW(TAG, "ON commands suppressed - run MAPTEST/PCF_TEST then set "
                      "WEIGHT_DEMO_POLARITY_VERIFIED");
    }
}

esp_err_t relay_set(uint8_t index, bool on)
{
    if (index >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Attribute every command on the dosing channels to the backend that will
     * actually carry it. There is no silent fallback: when the expander is
     * active these indices never reach GPIO18/19 (index_is_on_expander gates
     * both paths), and when it is not, the init-time WARN already said so.
     * Transitions log at INFO, repeats at DEBUG. */
    if (index <= RELAY_INDEX_FINE) {
        bool to_expander = index_is_on_expander(index);
        if (on != s_state[index]) {
            ESP_LOGI(TAG, "relay %u -> %s via %s", (unsigned)(index + 1U),
                     on ? "ON" : "OFF",
                     to_expander ? "I2C expander 0x22" : "direct GPIO (fallback)");
        } else {
            ESP_LOGD(TAG, "relay %u -> %s (repeat) via %s", (unsigned)(index + 1U),
                     on ? "ON" : "OFF",
                     to_expander ? "I2C expander 0x22" : "direct GPIO (fallback)");
        }
    }

    if (index_is_on_expander(index)) {
        if (on && !relay_expander_actuation_allowed()) {
            relay_log_suppressed();
            return ESP_ERR_INVALID_STATE;  /* zero bus traffic */
        }
        /* Shadow read-modify-write + bus write under the I2C mutex (a blocking
         * bus transaction can never run inside the GPIO spinlock). The
         * commanded-state cache follows the write result, so a failed write is
         * reported up and never cached as done. */
        bool before;
        esp_err_t ierr;
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        before = s_state[index];
        ierr = relay_expander_set_channel(index, on);
        if (ierr == ESP_OK) s_state[index] = on;
        /* H1: a failed write may still have latched. The expander flags that as
         * UNKNOWN; report ON (conservative) until a write ACKs. A gated refusal
         * does no bus write, leaves the flag alone and the cache unchanged. */
        else if (relay_expander_state_unknown()) s_state[index] = true;
        xSemaphoreGive(s_i2c_mutex);

        if (ierr != ESP_OK) {
            ESP_LOGE(TAG, "relay %u I2C write failed: %s",
                     (unsigned)(index + 1U), esp_err_to_name(ierr));
            return ierr;
        }
        if (before != on) {
            ESP_LOGD(TAG, "Relay %u -> %s (I2C 0x%02x)", (unsigned)(index + 1U),
                     on ? "ON" : "OFF", (unsigned)RELAY_EXPANDER_ADDR);
        }
        return ESP_OK;
    }

    bool changed = false;
    esp_err_t err = ESP_OK;

    /* Atomic: the cache and the pad can never disagree after this block. */
    portENTER_CRITICAL(&s_lock);
    if (s_state[index] != on) {
        s_state[index] = on;
        err = gpio_set_level(s_relay_gpio[index],
                             on ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay %u (GPIO%d) write failed: %s",
                 (unsigned)(index + 1U), relay_gpio_for_index(index),
                 esp_err_to_name(err));
        return err;
    }

    if (changed) {
        /* Per-channel transitions stay at debug level: a group crossing fires
         * up to twelve of them, and the caller logs one INFO line for the
         * group transition instead. */
        ESP_LOGD(TAG, "Relay %u -> %s", (unsigned)(index + 1U),
                 on ? "ON" : "OFF");
    }
    return ESP_OK;
}

/* Shared group command. The confirmed expander is driven first (ungated for
 * OFF, gated for ON), then the remaining channels through the GPIO path. Any
 * index the expander owns is SKIPPED on the GPIO side, so Relay 1/2 are never
 * driven as AO GPIOs while the confirmed I2C path is live. */
static esp_err_t relay_group(bool on)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t first_error = ESP_OK;

    if (s_use_i2c) {
        if (on && !relay_expander_actuation_allowed()) {
            relay_log_suppressed();
        } else {
            esp_err_t err;
            xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
            err = on ? relay_expander_all_on() : relay_expander_all_off();
            expander_cache_follow(err, on);
            xSemaphoreGive(s_i2c_mutex);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "group I2C write failed: %s", esp_err_to_name(err));
                if (first_error == ESP_OK) first_error = err;
            }
        }
    }

    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        if (index_is_on_expander(i)) continue;  /* owned by the expander */
        esp_err_t err = relay_set(i, on);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
    }
    return first_error;
}

esp_err_t relay_all_on(void)
{
    return relay_group(true);
}

esp_err_t relay_all_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* The de-energize path must succeed even when ON commands are gated, so it
     * drives the expander directly rather than through relay_group()'s gate.
     * relay_expander_all_off() is ungated by design: the inactive level is the
     * PCF8574's own power-up state under the measured ACTIVE-HIGH polarity, so
     * this write can never energize anything. */
    esp_err_t first_error = ESP_OK;

    if (s_use_i2c) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        esp_err_t err = relay_expander_all_off();
        /* The cache follows the write RESULT, so a failed de-energize is not
         * recorded as done and the next tick retries it (H1: an UNKNOWN state is
         * always actually written, never short-circuited). */
        expander_cache_follow(err, false);
        xSemaphoreGive(s_i2c_mutex);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "group I2C OFF failed: %s", esp_err_to_name(err));
            first_error = err;
        }
        /* Do NOT return here: a failed expander write must not also strand the
         * channels served by the GPIO path, which are independent outputs. */
    }

    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        if (index_is_on_expander(i)) continue;
        esp_err_t err = relay_set(i, false);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
    }
    return first_error;
}

esp_err_t relay_force_all_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t first_error = ESP_OK;

    if (s_use_i2c) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        esp_err_t err = relay_expander_force_all_off();
        expander_cache_follow(err, false);
        xSemaphoreGive(s_i2c_mutex);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "forced group I2C OFF failed: %s", esp_err_to_name(err));
            first_error = err;
        }
    }

    /* GPIO-served channels: the pad write cannot fail and the cache/pad pair is
     * updated atomically, so the cache-checked path is already reliable. */
    for (uint8_t i = 0U; i < RELAY_COUNT; i++) {
        if (index_is_on_expander(i)) continue;
        esp_err_t err = relay_set(i, false);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
    }
    return first_error;
}

bool relay_actuation_allowed(void)
{
    return !s_use_i2c || relay_expander_actuation_allowed();
}

bool relay_get_state(uint8_t index)
{
    if (index >= RELAY_COUNT) return false;

    bool state;
    portENTER_CRITICAL(&s_lock);
    state = s_state[index];
    portEXIT_CRITICAL(&s_lock);
    return state;
}

bool relay_backend_is_i2c(void)
{
    return s_use_i2c;
}

int relay_describe_channel(uint8_t index, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0U || index >= RELAY_COUNT) return 0;

    uint8_t bit = 0U;
    bool on_expander = index_is_on_expander(index) &&
                       relay_expander_bit_for_index(index, &bit);

    int n;
    if (on_expander) {
        n = snprintf(buf, cap, "expander 0x%02x bit%u (ack=%d)",
                     (unsigned)RELAY_EXPANDER_ADDR, (unsigned)bit,
                     (int)relay_expander_last_write_ok(index));
    } else if (relay_expander_backend_active()) {
        /* The confirmed expander is live but this channel is not one of its
         * outputs, so it falls to the reference-schematic GPIO path. Saying so
         * stops a bench reader assuming Relay N is on the expander. */
        n = snprintf(buf, cap, "GPIO%d (not on expander 0x%02x)",
                     relay_gpio_for_index(index), (unsigned)RELAY_EXPANDER_ADDR);
    } else {
        n = snprintf(buf, cap, "GPIO%d", relay_gpio_for_index(index));
    }

    if (n < 0) return 0;
    return (n >= (int)cap) ? (int)cap - 1 : n;
}

void relay_backend_run_map_test(void)
{
    relay_expander_map_test();
}

void relay_backend_run_pcf_test(void)
{
    relay_expander_pcf_test();
}

void relay_backend_run_act_test(void)
{
    relay_expander_act_test();
}

bool relay_pad_is_energized(uint8_t index)
{
    if (index >= RELAY_COUNT) return false;
    if (index_is_on_expander(index)) {
        /* DOCUMENTED SUBSTITUTION: there is no pad readback on the I2C
         * backend (a re-read would add bus traffic for no safety gain).
         * Reports the cached CONFIRMED wire state: the shadow bit is at the
         * energized level AND the last write ACKed. */
        /* H1: after a failed write the shadow proves nothing; report energized. */
        return relay_expander_state_unknown() ||
               (relay_expander_channel_on(index) &&
                relay_expander_last_write_ok(index));
    }
    return gpio_get_level(s_relay_gpio[index]) == RELAY_ACTIVE_LEVEL;
}