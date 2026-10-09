#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"  /* esp_read_mac, ESP_MAC_WIFI_STA */
#include <stdio.h>    /* snprintf */
#include <string.h>
#include <stdlib.h>   /* strtoul, strtol, strtof */

/* --------------------------------------------------------------------------
 * Private schema types — not exposed in the public header.
 * The TYPE column tokens in NVS_KEY_TABLE (NVS_T_STR, etc.) resolve here.
 * -------------------------------------------------------------------------- */
typedef enum {
    NVS_T_STR   = 0,   /* nvs_set_str  / nvs_get_str          */
    NVS_T_U32   = 1,   /* nvs_set_u32  / nvs_get_u32          */
    NVS_T_I32   = 2,   /* nvs_set_i32  / nvs_get_i32          */
    NVS_T_FLOAT = 3,   /* nvs_set_blob / nvs_get_blob (4 B)   */
    NVS_T_BLOB  = 4,
} nvs_val_type_t;

typedef struct {
    const char    *key;
    nvs_val_type_t type;
    const char    *default_str;  /* "" = no default; key must be provisioned */
} nvs_schema_entry_t;

/* Auto-generate s_schema[] from NVS_KEY_TABLE.
 * Adding a row to the table in nvs_config.h automatically adds it here. */
#define _NVS_SCHEMA_ROW(suffix, key_str, type, dflt)  { key_str, type, dflt },
static const nvs_schema_entry_t s_schema[] = {
    NVS_KEY_TABLE(_NVS_SCHEMA_ROW)
};
#undef _NVS_SCHEMA_ROW

static const char *TAG = "NVS_CFG";

static nvs_handle_t s_handle       = 0U;
static bool         s_initialized  = false;

/* --------------------------------------------------------------------------
 * nvs_config_init
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(TAG, "NVS partition corrupted — erasing and re-initialising");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s",
                 NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS namespace '%s' ready", NVS_NAMESPACE);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * nvs_config_key_exists
 * -------------------------------------------------------------------------- */
bool nvs_config_key_exists(const char *key)
{
    if (!s_initialized || (key == NULL)) {
        return false;
    }
    /* Probe with zero-length buffer — OK is "key exists with stored data",
     * INVALID_LENGTH also means the key exists (just needs a bigger buf). */
    size_t required = 0U;
    esp_err_t ret   = nvs_get_str(s_handle, key, NULL, &required);
    return ((ret == ESP_OK) || (ret == ESP_ERR_NVS_INVALID_LENGTH));
}

/* --------------------------------------------------------------------------
 * nvs_config_get_str
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_get_str(const char *key, char *out_buf, size_t buf_len)
{
    if (!s_initialized || (key == NULL) ||
        (out_buf == NULL) || (buf_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_get_str(s_handle, key, out_buf, &buf_len);
    if ((ret != ESP_OK) && (ret != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGD(TAG, "get_str '%s': %s", key, esp_err_to_name(ret));
    }
    return ret;
}

/* --------------------------------------------------------------------------
 * nvs_config_set_str
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_set_str(const char *key, const char *value)
{
    if (!s_initialized || (key == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_set_str(s_handle, key, value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str '%s' failed: %s", key, esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_commit(s_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Stored '%s' in NVS", key);
    }
    return ret;
}

/* --------------------------------------------------------------------------
 * nvs_config_get_u32
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_get_u32(const char *key, uint32_t *out_val)
{
    if (!s_initialized || (key == NULL) || (out_val == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_u32(s_handle, key, out_val);
}

/* --------------------------------------------------------------------------
 * nvs_config_set_u32
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_set_u32(const char *key, uint32_t value)
{
    if (!s_initialized || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_set_u32(s_handle, key, value);
    if (ret != ESP_OK) {
        return ret;
    }
    return nvs_commit(s_handle);
}

/* --------------------------------------------------------------------------
 * nvs_config_get_i32 / nvs_config_set_i32
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_get_i32(const char *key, int32_t *out_val)
{
    if (!s_initialized || (key == NULL) || (out_val == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_get_i32(s_handle, key, out_val);
}

esp_err_t nvs_config_set_i32(const char *key, int32_t value)
{
    if (!s_initialized || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_set_i32(s_handle, key, value);
    if (ret != ESP_OK) {
        return ret;
    }
    return nvs_commit(s_handle);
}

/* --------------------------------------------------------------------------
 * nvs_config_get_float / nvs_config_set_float
 * Floats are stored as 4-byte blobs to avoid floating-point string rounding.
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_get_float(const char *key, float *out_val)
{
    if (!s_initialized || (key == NULL) || (out_val == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t    sz  = sizeof(float);
    esp_err_t ret = nvs_get_blob(s_handle, key, out_val, &sz);
    if ((ret == ESP_OK) && (sz != sizeof(float))) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    return ret;
}

esp_err_t nvs_config_set_float(const char *key, float value)
{
    if (!s_initialized || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = nvs_set_blob(s_handle, key, &value, sizeof(float));
    if (ret != ESP_OK) {
        return ret;
    }
    return nvs_commit(s_handle);
}

esp_err_t nvs_config_get_blob(const char *key, void *buf, size_t *len)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!s_initialized || (key == NULL) || (len == NULL)) {
        goto cleanup;
    }
    if ((buf == NULL) && (*len != 0U)) {
        goto cleanup;
    }
    if (*len > NVS_CONFIG_BLOB_MAX) {
        *len = NVS_CONFIG_BLOB_MAX;
    }
    ret = nvs_get_blob(s_handle, key, buf, len);
    if ((ret != ESP_OK) && (ret != ESP_ERR_NVS_NOT_FOUND) && (ret != ESP_ERR_NVS_INVALID_LENGTH)) {
        ESP_LOGW(TAG, "get_blob '%s': %s", key, esp_err_to_name(ret));
    }

cleanup:
    return ret;
}

esp_err_t nvs_config_set_blob(const char *key, const void *buf, size_t len)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!s_initialized || (key == NULL) || (buf == NULL) || (len == 0U) || (len > NVS_CONFIG_BLOB_MAX)) {
        goto cleanup;
    }
    ret = nvs_set_blob(s_handle, key, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob '%s' failed: %s", key, esp_err_to_name(ret));
        goto cleanup;
    }
    ret = nvs_commit(s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "commit '%s' failed: %s", key, esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Stored blob '%s' (%u B) in NVS", key, (unsigned)len);

cleanup:
    return ret;
}

esp_err_t nvs_config_erase_key(const char *key)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!s_initialized || (key == NULL)) {
        goto cleanup;
    }
    ret = nvs_erase_key(s_handle, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        goto cleanup;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key '%s' failed: %s", key, esp_err_to_name(ret));
        goto cleanup;
    }
    ret = nvs_commit(s_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Erased '%s' from NVS", key);
    }

cleanup:
    return ret;
}

/* --------------------------------------------------------------------------
 * Private helpers for schema-driven operations
 * -------------------------------------------------------------------------- */

/* Check whether a key already has a stored value, using the correct NVS
 * getter for the entry's type so non-string keys are probed correctly. */
static bool key_exists_typed(const nvs_schema_entry_t *e)
{
    esp_err_t ret;
    switch (e->type) {
        case NVS_T_STR: {
            size_t len = 0U;
            ret = nvs_get_str(s_handle, e->key, NULL, &len);
            return ((ret == ESP_OK) || (ret == ESP_ERR_NVS_INVALID_LENGTH));
        }
        case NVS_T_U32: {
            uint32_t v = 0U;
            ret = nvs_get_u32(s_handle, e->key, &v);
            return (ret == ESP_OK);
        }
        case NVS_T_I32: {
            int32_t v = 0;
            ret = nvs_get_i32(s_handle, e->key, &v);
            return (ret == ESP_OK);
        }
        case NVS_T_FLOAT: {
            float  v  = 0.0f;
            size_t sz = sizeof(float);
            ret = nvs_get_blob(s_handle, e->key, &v, &sz);
            return (ret == ESP_OK);
        }
        case NVS_T_BLOB: {
            size_t sz = 0U;
            ret = nvs_get_blob(s_handle, e->key, NULL, &sz);
            return ((ret == ESP_OK) || (ret == ESP_ERR_NVS_INVALID_LENGTH));
        }
        default:
            return false;
    }
}

/* Write a value from its string representation using the schema type.
 * Caller is responsible for calling nvs_commit() afterwards. */
static esp_err_t write_from_str(const nvs_schema_entry_t *e,
                                 const char *value_str)
{
    esp_err_t ret;
    switch (e->type) {
        case NVS_T_STR: {
            ret = nvs_set_str(s_handle, e->key, value_str);
            break;
        }
        case NVS_T_U32: {
            char *end = NULL;
            unsigned long v = strtoul(value_str, &end, 10);
            if (end == value_str) { return ESP_ERR_INVALID_ARG; }
            ret = nvs_set_u32(s_handle, e->key, (uint32_t)v);
            break;
        }
        case NVS_T_I32: {
            char *end = NULL;
            long v = strtol(value_str, &end, 10);
            if (end == value_str) { return ESP_ERR_INVALID_ARG; }
            ret = nvs_set_i32(s_handle, e->key, (int32_t)v);
            break;
        }
        case NVS_T_FLOAT: {
            char *end = NULL;
            float v = strtof(value_str, &end);
            if (end == value_str) { return ESP_ERR_INVALID_ARG; }
            ret = nvs_set_blob(s_handle, e->key, &v, sizeof(float));
            break;
        }
        case NVS_T_BLOB:
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return nvs_commit(s_handle);
}

/* --------------------------------------------------------------------------
 * nvs_config_load_defaults
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_load_defaults(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t schema_count = (uint32_t)(sizeof(s_schema) / sizeof(s_schema[0]));
    uint32_t written      = 0U;

    for (uint32_t i = 0U; i < schema_count; i++) {
        const nvs_schema_entry_t *e = &s_schema[i];

        /* Skip keys that have no default (credentials set externally) */
        if (e->default_str[0] == '\0') {
            continue;
        }

        /* Skip keys that already have a stored value (preserve user data) */
        if (key_exists_typed(e)) {
            continue;
        }

        esp_err_t ret = write_from_str(e, e->default_str);
        if (ret == ESP_OK) {
            /* Do not log key/value pairs: some defaults are secrets (WiFi
             * password). Only the aggregate count is logged below. */
            written++;
        } else {
            ESP_LOGW(TAG, "Failed default for '%s': %s",
                     e->key, esp_err_to_name(ret));
        }
    }

    if (written > 0U) {
        ESP_LOGI(TAG, "%lu default(s) written on first boot", (unsigned long)written);
    } else {
        ESP_LOGD(TAG, "All NVS keys already present — no defaults applied");
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * nvs_config_ensure_device_id
 *
 * device_id has no factory default, so on first boot it is absent/empty. Derive
 * it from the WiFi STA MAC (read from efuse — no WiFi init required) as
 * "bits-<12 lowercase hex>" and persist it. An id already in NVS is preserved,
 * so a manually-set device_id is never overwritten.
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_ensure_device_id(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    char existing[40] = {0};
    esp_err_t ret = nvs_config_get_str(NVS_KEY_DEVICE_ID, existing, sizeof(existing));
    if ((ret == ESP_OK) && (existing[0] != '\0')) {
        ESP_LOGI(TAG, "device_id: %s", existing);
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char id[18];  /* "bits-" (5) + 12 hex + NUL */
    (void)snprintf(id, sizeof(id), "bits-%02x%02x%02x%02x%02x%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ret = nvs_config_set_str(NVS_KEY_DEVICE_ID, id);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "device_id derived from WiFi STA MAC: %s", id);
    }
    return ret;
}

/* --------------------------------------------------------------------------
 * nvs_config_set_by_key
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_set_by_key(const char *key, const char *value_str)
{
    if (!s_initialized || (key == NULL) || (value_str == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t schema_count = (uint32_t)(sizeof(s_schema) / sizeof(s_schema[0]));
    for (uint32_t i = 0U; i < schema_count; i++) {
        if (strcmp(s_schema[i].key, key) == 0) {
            esp_err_t ret = write_from_str(&s_schema[i], value_str);
            if (ret == ESP_OK) {
                /* Log the key only — the value may be a secret. */
                ESP_LOGI(TAG, "Updated via key: [%s]", key);
            }
            return ret;
        }
    }

    ESP_LOGW(TAG, "set_by_key: '%s' not found in schema", key);
    return ESP_ERR_NVS_NOT_FOUND;
}

/* --------------------------------------------------------------------------
 * nvs_config_erase_all
 * -------------------------------------------------------------------------- */
esp_err_t nvs_config_erase_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = nvs_erase_all(s_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_commit(s_handle);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' erased (factory reset)", NVS_NAMESPACE);
    }
    return ret;
}
