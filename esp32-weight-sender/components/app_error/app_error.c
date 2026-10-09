#include "app_error.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Error string lookup table — extend when new error codes are added.
 * -------------------------------------------------------------------------- */
static const struct {
    esp_err_t   code;
    const char *str;
} s_err_table[] = {
    { APP_OK,                   "OK"                 },
    { APP_ERR_TIMEOUT,          "TIMEOUT"            },
    { APP_ERR_INVALID_ARG,      "INVALID_ARG"        },
    { APP_ERR_MODBUS_CRC,       "MODBUS_CRC"         },
    { APP_ERR_MODBUS_TIMEOUT,   "MODBUS_TIMEOUT"     },
    { APP_ERR_MODBUS_EXCEPTION, "MODBUS_EXCEPTION"   },
    { APP_ERR_NET_NO_IP,        "NET_NO_IP"          },
    { APP_ERR_MQTT_DISCONNECT,  "MQTT_DISCONNECT"    },
    { APP_ERR_OTA_FAILED,       "OTA_FAILED"         },
    { APP_ERR_CONFIG_INVALID,   "CONFIG_INVALID"     },
    { APP_ERR_UART,             "UART"               },
};

#define ERR_TABLE_SIZE ((uint8_t)(sizeof(s_err_table) / sizeof(s_err_table[0])))

const char *app_err_to_str(esp_err_t err)
{
    uint8_t i = 0U;

    for (i = 0U; i < ERR_TABLE_SIZE; i++) {
        if (s_err_table[i].code == err) {
            return s_err_table[i].str;
        }
    }
    return "UNKNOWN";
}
