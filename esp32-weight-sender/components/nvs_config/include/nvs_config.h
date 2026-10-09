#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * NVS KEY TABLE â€” single source of truth for all persisted variables.
 *
 * To add a new NVS variable: add ONE row to this table. Nothing else needed.
 * The key string constant, schema entry, and set_by_key dispatch are all
 * auto-generated from this table at compile time.
 *
 * Columns:
 *   X(SUFFIX,  "nvs_key_str",  TYPE,         "default_or_empty")
 *
 *   SUFFIX       â†’ produces NVS_KEY_<SUFFIX>, usable as (const char *)
 *   nvs_key_str  â†’ actual key written to flash  (NVS hard limit: 15 chars)
 *   TYPE         â†’ NVS_T_STR | NVS_T_U32 | NVS_T_I32 | NVS_T_FLOAT | NVS_T_BLOB
 *   default      â†’ written once on first boot by nvs_config_load_defaults()
 *                  use "" to skip â€” key must be set via MQTT/REPL
 * ============================================================================ */

#define NVS_KEY_TABLE(X)                                                                            \
/*   SUFFIX        NVS key string   TYPE          Default ("" = must be provisioned) */             \
    X(WIFI_SSID_1, "wifi_ssid_1",   NVS_T_STR,   "")                                            \
    X(WIFI_PASS_1, "wifi_pass_1",   NVS_T_STR,   "")                                            \
    X(WIFI_SSID_2, "wifi_ssid_2",   NVS_T_STR,   "")                                            \
    X(WIFI_PASS_2, "wifi_pass_2",   NVS_T_STR,   "")                                            \
    X(DEVICE_ID,   "device_id",     NVS_T_STR,   "")                                            \
    X(FORCE_PROV,  "force_prov",    NVS_T_U32,   "0")                                           \
    X(SCALE_ID,    "scale_id",      NVS_T_STR,   "SCALE1")                                      \
    /* Sender only: which RS-485 channel carries the scale. 0 = use the Kconfig  */                 \
    /* default (WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL), 1 = CH1/UART1, 2 = CH2/UART2. */                 \
    X(SCALE_CH,    "scale_ch",      NVS_T_U32,   "0")                                           \
    X(MQTT_CFG,    "mqtt_cfg",      NVS_T_BLOB,  "")
/* --------------------------------------------------------------------------
 * Auto-generate NVS_KEY_<SUFFIX> string constants from the table.
 *
 * The TYPE column is a private enum token (defined in nvs_config.c) â€” it is
 * intentionally ignored here so callers do not need to know about storage
 * internals. Only suffix and key_str are used in this expansion.
 *
 * 'static' + identical string content â†’ linker merges duplicates across TUs.
 * '__attribute__((unused))' suppresses -Wunused warnings in TUs that include
 * this header but do not reference every key.
 * -------------------------------------------------------------------------- */
#define _NVS_KEY_DECL(suffix, key_str, type, dflt) \
    static const char NVS_KEY_##suffix[] __attribute__((unused)) = key_str;
NVS_KEY_TABLE(_NVS_KEY_DECL)
#undef _NVS_KEY_DECL

/* NVS namespace â€” all keys live under this name in the partition */
#define NVS_NAMESPACE "scale_cfg"

/* --------------------------------------------------------------------------
 * Public API
 *
 * Prefer the typed get/set for explicit control.
 * Use nvs_config_set_by_key() for runtime updates (MQTT commands / REPL).
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_init       (void);
bool      nvs_config_key_exists (const char *key);

esp_err_t nvs_config_get_str    (const char *key, char    *out_buf, size_t buf_len);
esp_err_t nvs_config_set_str    (const char *key, const char *value);

esp_err_t nvs_config_get_u32    (const char *key, uint32_t *out_val);
esp_err_t nvs_config_set_u32    (const char *key, uint32_t  value);

esp_err_t nvs_config_get_i32    (const char *key, int32_t  *out_val);
esp_err_t nvs_config_set_i32    (const char *key, int32_t   value);

esp_err_t nvs_config_get_float  (const char *key, float    *out_val);
esp_err_t nvs_config_set_float  (const char *key, float     value);

#define NVS_CONFIG_BLOB_MAX 1024U

esp_err_t nvs_config_get_blob   (const char *key, void *buf, size_t *len);
esp_err_t nvs_config_set_blob   (const char *key, const void *buf, size_t len);
esp_err_t nvs_config_erase_key  (const char *key);

/**
 * @brief Write factory defaults for every absent key (first-boot only).
 *        Keys already in NVS are untouched. Call once after nvs_config_init().
 */
esp_err_t nvs_config_load_defaults(void);

/**
 * @brief Ensure device_id is set, deriving it from the WiFi STA MAC if absent.
 *
 *        On first boot device_id is empty (no default), so this reads the
 *        chip's WiFi STA MAC from efuse and stores it as "bits-<12 lowercase
 *        hex>" (e.g. "bits-a4cf12ab34cd"). A device_id already present in NVS
 *        (derived earlier or set manually) is left untouched, so a custom id
 *        survives. Call once after nvs_config_load_defaults().
 */
esp_err_t nvs_config_ensure_device_id(void);

/**
 * @brief Update any registered key from a string â€” type conversion is automatic.
 *        Intended for MQTT command handler and MicroPython REPL.
 *        Example: nvs_config_set_by_key("device_id", "vfd_002")
 * @return ESP_ERR_NVS_NOT_FOUND if key is not registered in NVS_KEY_TABLE.
 */
esp_err_t nvs_config_set_by_key(const char *key, const char *value_str);

/**
 * @brief Erase all keys in the project namespace.
 *        Call esp_restart() afterwards to apply factory defaults.
 */
esp_err_t nvs_config_erase_all(void);

#endif /* NVS_CONFIG_H */
