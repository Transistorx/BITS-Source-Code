#pragma once
#include "esp_err.h"

typedef enum {
    LED_OFF          = 0,
    LED_CONNECTING   = 1,
    LED_CONNECTED    = 2,
    LED_PROVISIONING = 3,
    LED_MODE_COUNT
} led_mode_t;

esp_err_t led_status_init(void);
void      led_status_set(led_mode_t mode);
