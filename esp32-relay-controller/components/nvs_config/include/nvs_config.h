#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * NVS KEY TABLE — single source of truth for all persisted variables.
 *
 * To add a new NVS variable: add ONE row to this table. Nothing else needed.
 * The key string constant, schema entry, and set_by_key dispatch are all
 * auto-generated from this table at compile time.
 *
 * Columns:
 *   X(SUFFIX,  "nvs_key_str",  TYPE,         "default_or_empty")
 *
 *   SUFFIX       → produces NVS_KEY_<SUFFIX>, usable as (const char *)
 *   nvs_key_str  → actual key written to flash  (NVS hard limit: 15 chars)
 *   TYPE         → NVS_T_STR | NVS_T_U32 | NVS_T_I32 | NVS_T_FLOAT
 *   default      → written once on first boot by nvs_config_load_defaults()
 *                  use "" to skip — key must be set via MQTT/REPL
 * ============================================================================ */

/* Relay only: WEIGHT_XPORT is the logical "weight_transport" ("ws" default |
 * "mqtt"); NVS keys max 15 chars and that name is 16. PEER_SENDER_ID is the
 * sender's device id for cas/<peer>/weight/ctl (must be provisioned for mqtt).
 * RUN_MQTT (default 0) switches the run lifecycle to MQTT (CONTRACT 9.2);
 * REQ_PIN_PROFILE (default 0) refuses a JOB without a pinned profile (CONTRACT 9.3).
 * HASH_CHECK (default 0) verifies profile_hash and reports applied_profile (CONTRACT 9.3). */
#define NVS_KEY_TABLE(X)                                                                          \
/*   SUFFIX        NVS key string   TYPE          Default ("" = must be provisioned) */             \
    X(WIFI_SSID_1, "wifi_ssid_1",   NVS_T_STR,   "")                                            \
    X(WIFI_PASS_1, "wifi_pass_1",   NVS_T_STR,   "")                                            \
    X(WIFI_SSID_2, "wifi_ssid_2",   NVS_T_STR,   "")                                            \
    X(WIFI_PASS_2, "wifi_pass_2",   NVS_T_STR,   "")                                            \
    X(DEVICE_ID,   "device_id",     NVS_T_STR,   "")                                            \
    X(FORCE_PROV,  "force_prov",    NVS_T_U32,   "0")                                           \
    X(WEIGHT_XPORT, "weight_xport", NVS_T_STR,   "ws")                                          \
    X(PEER_SENDER_ID, "peer_sender_id", NVS_T_STR, "")                                          \
    X(RUN_MQTT,    "run_mqtt",      NVS_T_U32,   "0")                                           \
    X(REQ_PIN_PROFILE, "req_pin_prof", NVS_T_U32, "0")                                          \
    X(HASH_CHECK,  "hash_check",    NVS_T_U32,   "0")                                           \
    X(SCALE_ID,    "scale_id",      NVS_T_STR,   "SCALE1")                                      \
    X(RDY_STABLE_S, "rdy_stable_s", NVS_T_U32,   "3")                                           \
    X(RDY_MAX_AGE_S, "ready_max_age_s", NVS_T_U32, "120")                                       \
    X(RECONFIRM_SAME, "reconfirm_same", NVS_T_U32, "1")                                         \
    X(LEGACY_READY, "legacy_ready", NVS_T_U32,   "1")                                           \
    X(MOVE_TIMEOUT_S, "scale_move_to_s", NVS_T_U32, "600")                                      \
    X(START_MAX_G, "start_max_g",   NVS_T_U32,   "500")                                         \
    X(DROP_TRIP_G, "drop_trip_g",   NVS_T_U32,   "200")                                         \
    X(NP_LEARN_S,  "np_learn_s",    NVS_T_U32,   "10")                                          \
    X(NP_ABS_FLOOR_G, "np_abs_floor_g", NVS_T_U32, "10")                                        \
    X(SCALE_RES_G, "scale_res_g",   NVS_T_U32,   "2")
/* Single-scale operation (CONTRACT 9.11), relay only. ALL numeric values here are
 * UNVALIDATED conservative defaults pending field data: SCALE_ID (expected sender
 * scale_id; mismatch rejects the frame), RDY_STABLE_S (stable run required before a
 * READY), RDY_MAX_AGE_S (READY age limit), RECONFIRM_SAME (1 = a fresh READY is
 * needed between jobs on the same pump), LEGACY_READY (1 = accept a READY with only
 * {channel}; migration only), MOVE_TIMEOUT_S, START_MAX_G (0 disables), DROP_TRIP_G,
 * NP_LEARN_S / NP_ABS_FLOOR_G (NO_PROGRESS), SCALE_RES_G (scale resolution). */
/* --------------------------------------------------------------------------
 * Auto-generate NVS_KEY_<SUFFIX> string constants from the table.
 *
 * The TYPE column is a private enum token (defined in nvs_config.c) — it is
 * intentionally ignored here so callers do not need to know about storage
 * internals. Only suffix and key_str are used in this expansion.
 *
 * 'static' + identical string content → linker merges duplicates across TUs.
 * '__attribute__((unused))' suppresses -Wunused warnings in TUs that include
 * this header but do not reference every key.
 * -------------------------------------------------------------------------- */
#define _NVS_KEY_DECL(suffix, key_str, type, dflt) \
    static const char NVS_KEY_##suffix[] __attribute__((unused)) = key_str;
NVS_KEY_TABLE(_NVS_KEY_DECL)
#undef _NVS_KEY_DECL

/* NVS namespace — all keys live under this name in the partition */
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
 * @brief Update any registered key from a string — type conversion is automatic.
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
