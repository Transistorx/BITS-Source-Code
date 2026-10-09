#include "led_status.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

/* The status-LED pin is a project-level Kconfig choice:
 *   CONFIG_LED_STATUS_GPIO >= 0 : drive that GPIO (module onboard LED = GPIO2)
 *   CONFIG_LED_STATUS_GPIO == -1: this board has no status LED.
 *
 * The 12-relay controller sets -1: its GPIO2 is AO12, relay_driver must be
 * the pin's single owner (a second gpio_config() claim logs "gpio: conflict
 * found for GPIO[2]", and the LED task would fight the all-relays-off
 * failsafe), so provisioning/connect hints are log-only on that device.
 * The weight sender keeps GPIO2 — it has no relay outputs.
 *
 * The fallback keeps this component buildable in projects without Kconfig
 * (the QEMU test builds), where it is compiled but never initialised. */
#ifndef CONFIG_LED_STATUS_GPIO
#define CONFIG_LED_STATUS_GPIO 2
#endif

#if CONFIG_LED_STATUS_GPIO >= 0

#define LED_GPIO ((gpio_num_t)CONFIG_LED_STATUS_GPIO)

typedef struct {
    uint32_t on_ms;
    uint32_t off_ms;
} led_pattern_t;

/* on_ms==0 → always off; off_ms==0 → always on; both nonzero → blink */
static const led_pattern_t s_patterns[LED_MODE_COUNT] = {
    [LED_OFF]          = {   0U, 100U },   /* off                    */
    [LED_CONNECTING]   = { 500U, 500U },   /* 1 Hz slow blink        */
    [LED_CONNECTED]    = { 100U,   0U },   /* solid on               */
    [LED_PROVISIONING] = { 100U, 100U },   /* 5 Hz fast blink        */
};

/* volatile: single writer (led_status_set), single reader (led_task).
 * 32-bit aligned enum — read/write is atomic on Xtensa. */
static volatile led_mode_t s_mode = LED_OFF;

static void led_task(void *arg)
{
    bool led_on = false;

    (void)arg;
    while (1) {
        const led_pattern_t *pat = &s_patterns[(uint8_t)s_mode];

        if (pat->on_ms == 0U) {
            (void)gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100U));
        } else if (pat->off_ms == 0U) {
            (void)gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100U));
        } else {
            led_on = !led_on;
            (void)gpio_set_level(LED_GPIO, led_on ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(led_on ? pat->on_ms : pat->off_ms));
        }
    }
}

esp_err_t led_status_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    (void)gpio_set_level(LED_GPIO, 0);

    BaseType_t task_ret = xTaskCreate(led_task, "led_status", 2048U, NULL, 2U, NULL);
    if (task_ret != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void led_status_set(led_mode_t mode)
{
    if ((uint8_t)mode < (uint8_t)LED_MODE_COUNT) {
        s_mode = mode;
    }
}

#else /* CONFIG_LED_STATUS_GPIO < 0: no status LED on this board. */

esp_err_t led_status_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void led_status_set(led_mode_t mode)
{
    (void)mode;
}

#endif /* CONFIG_LED_STATUS_GPIO */
