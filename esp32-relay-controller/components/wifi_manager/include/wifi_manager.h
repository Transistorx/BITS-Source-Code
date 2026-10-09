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

/* Invoked from the Wi-Fi event handler when the station disconnects. */
void wifi_manager_set_link_lost_callback(void (*cb)(void));

#endif /* WIFI_MANAGER_H */