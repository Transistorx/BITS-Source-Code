#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads Wi-Fi credentials from NVS (written by the provisioning portal).
 * When no credentials are stored, Wi-Fi is skipped entirely — no radio, no
 * connect-retry loop — and the status LED fast-blinks as the "needs
 * provisioning" hint. */
esp_err_t wifi_manager_start(void);
bool wifi_manager_wait_connected(uint32_t timeout_ms);
/* True only when the STA was started with stored credentials. */
bool wifi_manager_is_started(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_get_ip(char *buf, size_t cap);

/* Advertises <CONFIG_WEIGHT_DEMO_MDNS_HOSTNAME>.local so ESP32 #2 can resolve
 * this device without a hardcoded address. */
esp_err_t wifi_manager_start_mdns(void);

#endif /* WIFI_MANAGER_H */