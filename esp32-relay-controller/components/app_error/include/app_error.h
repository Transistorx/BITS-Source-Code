#ifndef APP_ERROR_H
#define APP_ERROR_H

#include "esp_err.h"
#include "esp_log.h"

/* --------------------------------------------------------------------------
 * Project-wide error codes
 * Values above 0x3000 are project-specific (do not overlap with ESP-IDF range)
 * -------------------------------------------------------------------------- */
typedef enum {
    APP_OK                   = 0,
    APP_ERR_TIMEOUT          = 0x3001,
    APP_ERR_INVALID_ARG      = 0x3002,
    APP_ERR_MODBUS_CRC       = 0x3003,
    APP_ERR_MODBUS_TIMEOUT   = 0x3004,
    APP_ERR_MODBUS_EXCEPTION = 0x3005,
    APP_ERR_NET_NO_IP        = 0x3006,
    APP_ERR_MQTT_DISCONNECT  = 0x3007,
    APP_ERR_OTA_FAILED       = 0x3008,
    APP_ERR_CONFIG_INVALID   = 0x3009,
    APP_ERR_UART             = 0x300A,
} app_err_t;

/* --------------------------------------------------------------------------
 * CHECK_ERR — log source location and return on failure.
 * Use in functions that return esp_err_t.
 * -------------------------------------------------------------------------- */
#define CHECK_ERR(tag, x)                                                    \
    do {                                                                     \
        esp_err_t _err_rc = (x);                                             \
        if (_err_rc != ESP_OK) {                                             \
            ESP_LOGE((tag), "Error 0x%x at %s:%d", _err_rc,                 \
                     __FILE__, __LINE__);                                    \
            return _err_rc;                                                  \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------------------
 * LOG_ERR — log error but do not return. Use in void functions or when
 * the caller handles the error inline.
 * -------------------------------------------------------------------------- */
#define LOG_ERR(tag, x)                                                      \
    do {                                                                     \
        esp_err_t _err_rc = (x);                                             \
        if (_err_rc != ESP_OK) {                                             \
            ESP_LOGE((tag), "Error 0x%x at %s:%d", _err_rc,                 \
                     __FILE__, __LINE__);                                    \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------------------
 * app_err_to_str — human-readable string for an error code.
 * Returns "UNKNOWN" for codes not in the table.
 * -------------------------------------------------------------------------- */
const char *app_err_to_str(esp_err_t err);

#endif /* APP_ERROR_H */
