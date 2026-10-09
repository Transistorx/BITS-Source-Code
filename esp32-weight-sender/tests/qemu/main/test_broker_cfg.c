#include "test_broker_cfg.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "broker_cfg.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_config.h"
#include "test_harness.h"

#define FLASH_ERASED 0xFFU

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0U; i < n; i++) {
        if (b[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool is_fallback(broker_cfg_src_t s)
{
    return (s != BROKER_CFG_SRC_NVS) && (s != BROKER_CFG_SRC_KCONFIG);
}

static void test_crc_and_layout(void)
{
    test_check(broker_cfg_crc32("123456789", 9U) == 0xCBF43926U, "REQ-WMQ-23_crc32_ieee_check_vector");
    test_check(broker_cfg_crc32(NULL, 9U) == 0U, "REQ-WMQ-23_crc32_null_is_zero");
    test_check(sizeof(broker_cfg_rec_t) == 204U && offsetof(broker_cfg_rec_t, crc32) == 200U,
               "REQ-WMQ-23_record_layout_204_bytes_crc_last");
    test_check(strcmp(broker_cfg_src_name(BROKER_CFG_SRC_KCONFIG), "kconfig") == 0 &&
                   strcmp(broker_cfg_src_name(BROKER_CFG_SRC_NVS), "nvs") == 0 &&
                   strcmp(broker_cfg_src_name(BROKER_CFG_SRC_FALLBACK_ABSENT), "fallback_absent") == 0 &&
                   strcmp(broker_cfg_src_name(BROKER_CFG_SRC_FALLBACK_VERSION), "fallback_version") == 0 &&
                   strcmp(broker_cfg_src_name(BROKER_CFG_SRC_FALLBACK_CRC), "fallback_crc") == 0 &&
                   strcmp(broker_cfg_src_name(BROKER_CFG_SRC_FALLBACK_INVALID), "fallback_invalid") == 0 &&
                   strcmp(broker_cfg_src_name((broker_cfg_src_t)99), "unknown") == 0,
               "REQ-WMQ-24_src_name_table_complete_and_bounded");
}

static void test_power_loss(void)
{
    broker_cfg_rec_t old_rec;
    broker_cfg_rec_t new_rec;
    broker_cfg_rec_t out;
    uint8_t cell[sizeof(broker_cfg_rec_t)];

    test_check(broker_cfg_encode(&old_rec, "mqtt://old.lan:1883", "dev_old", "pw_old"), "REQ-WMQ-23_old_record_encodes");
    test_check(broker_cfg_encode(&new_rec, "mqtts://new.lan:8883", "dev_new", "pw_new"), "REQ-WMQ-23_new_record_encodes");

    int torn_ok = 1;
    for (size_t cut = 0U; cut < sizeof(cell); cut++) {
        memset(cell, FLASH_ERASED, sizeof(cell));
        memcpy(cell, &new_rec, cut);
        broker_cfg_src_t s = broker_cfg_decode(cell, sizeof(cell), &out);
        if (!is_fallback(s) || !all_zero(&out, sizeof(out))) {
            torn_ok = 0;
        }
    }
    test_check(torn_ok == 1, "REQ-WMQ-23_power_loss_at_every_byte_falls_back_with_zeroed_out");

    memset(cell, FLASH_ERASED, sizeof(cell));
    test_check(broker_cfg_decode(cell, sizeof(cell), &out) == BROKER_CFG_SRC_FALLBACK_CRC,
               "REQ-WMQ-23_erased_flash_pattern_reports_crc");

    int mixed_ok = 1;
    for (size_t cut = 1U; cut < sizeof(cell); cut++) {
        memcpy(cell, &old_rec, sizeof(cell));
        memcpy(cell, &new_rec, cut);
        broker_cfg_src_t s = broker_cfg_decode(cell, sizeof(cell), &out);
        if (s == BROKER_CFG_SRC_NVS) {
            if (memcmp(&out, &old_rec, sizeof(out)) != 0) {
                mixed_ok = 0;
            }
        } else if (!is_fallback(s)) {
            mixed_ok = 0;
        }
    }
    test_check(mixed_ok == 1, "REQ-WMQ-23_mixed_old_new_record_is_old_or_fallback_never_mix");

    memcpy(cell, &old_rec, sizeof(cell));
    test_check(broker_cfg_decode(cell, sizeof(cell), &out) == BROKER_CFG_SRC_NVS &&
                   strcmp(out.uri, "mqtt://old.lan:1883") == 0 && strcmp(out.user, "dev_old") == 0,
               "REQ-WMQ-23_failed_commit_keeps_old_record_intact");

    uint8_t shifted[sizeof(broker_cfg_rec_t) + 1U];
    memcpy(shifted + 1, &new_rec, sizeof(new_rec));
    test_check(broker_cfg_decode(shifted + 1, sizeof(new_rec), &out) == BROKER_CFG_SRC_NVS &&
                   strcmp(out.pass, "pw_new") == 0,
               "REQ-WMQ-23_unaligned_blob_decodes");

    uint8_t bigger[sizeof(broker_cfg_rec_t) + 8U];
    memset(bigger, 0, sizeof(bigger));
    memcpy(bigger, &new_rec, sizeof(new_rec));
    test_check(broker_cfg_decode(bigger, sizeof(bigger), &out) == BROKER_CFG_SRC_FALLBACK_VERSION,
               "REQ-WMQ-23_longer_record_reports_version");
    test_check(broker_cfg_decode(&new_rec, sizeof(new_rec) - 1U, &out) == BROKER_CFG_SRC_FALLBACK_VERSION,
               "REQ-WMQ-23_shorter_record_reports_version");
    test_check(broker_cfg_decode(&new_rec, sizeof(new_rec), NULL) == BROKER_CFG_SRC_FALLBACK_INVALID,
               "REQ-WMQ-23_null_out_is_invalid");
}

static void test_fields(void)
{
    broker_cfg_rec_t r;
    broker_cfg_rec_t out;

    test_check(broker_cfg_encode(&r, "mqtt://h", "dev_1", "") &&
                   broker_cfg_decode(&r, sizeof(r), &out) == BROKER_CFG_SRC_NVS && out.pass[0] == '\0' &&
                   strcmp(out.user, "dev_1") == 0,
               "REQ-WMQ-21_empty_password_with_user_allowed");
    test_check(broker_cfg_encode(&r, "mqtt://h", "", "") && broker_cfg_decode(&r, sizeof(r), &out) == BROKER_CFG_SRC_NVS &&
                   out.user[0] == '\0' && out.pass[0] == '\0',
               "REQ-WMQ-21_anonymous_record_allowed");

    broker_cfg_rec_t v = r;
    v.flags = 1U;
    v.crc32 = broker_cfg_crc32(&v, offsetof(broker_cfg_rec_t, crc32));
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_INVALID,
               "REQ-WMQ-23_nonzero_flags_rejected");

    v = r;
    memset(v.user, 'u', sizeof(v.user));
    v.crc32 = broker_cfg_crc32(&v, offsetof(broker_cfg_rec_t, crc32));
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_INVALID,
               "REQ-WMQ-22_unterminated_user_reports_invalid");

    memset(&out, 0xA5, sizeof(out));
    test_check(broker_cfg_decode(&v, sizeof(v), &out) != BROKER_CFG_SRC_NVS && all_zero(&out, sizeof(out)),
               "REQ-WMQ-27_out_zeroed_on_fallback");
    memset(&out, 0xA5, sizeof(out));
    broker_cfg_rec_t e;
    test_check(broker_cfg_encode(&e, "", "u", "p") && broker_cfg_decode(&e, sizeof(e), &out) == BROKER_CFG_SRC_KCONFIG &&
                   all_zero(&out, sizeof(out)),
               "REQ-WMQ-21_kconfig_record_returns_no_credentials");
    memset(&out, 0xA5, sizeof(out));
    test_check(broker_cfg_decode(NULL, 0U, &out) == BROKER_CFG_SRC_FALLBACK_ABSENT && all_zero(&out, sizeof(out)),
               "REQ-WMQ-21_absent_zeroes_out");

    memset(&e, 0xA5, sizeof(e));
    test_check(!broker_cfg_encode(&e, "mqtt://h", NULL, "p") && all_zero(&e, sizeof(e)),
               "REQ-WMQ-27_failed_encode_wipes_record");
    memset(&e, 0xA5, sizeof(e));
    char p64[65];
    memset(p64, 'p', 64U);
    p64[64] = '\0';
    test_check(!broker_cfg_encode(&e, "mqtt://h", "u", p64) && all_zero(&e, sizeof(e)),
               "REQ-WMQ-22_oversize_pass_wipes_record");
    char u40[41];
    memset(u40, 'u', 40U);
    u40[40] = '\0';
    test_check(!broker_cfg_encode(&e, "mqtt://h", u40, "p"), "REQ-WMQ-22_oversize_user_rejected");
    test_check(!broker_cfg_encode(&e, "mqtt://h", "u", NULL), "REQ-WMQ-22_null_pass_rejected");

    broker_cfg_rec_t w;
    (void)broker_cfg_encode(&w, "mqtt://h", "u", "secret");
    broker_cfg_wipe(&w);
    test_check(all_zero(&w, sizeof(w)), "REQ-WMQ-27_wipe_clears_record");
    broker_cfg_wipe(NULL);
}

static void test_uri_table(void)
{
    const char *good[] = {"mqtt://h.", "mqtt://-h", "mqtt://host:01883", "mqtts://1.2.3.4", "mqtt://a:65535",
                          "mqtt://localhost:1883"};
    for (size_t i = 0U; i < sizeof(good) / sizeof(good[0]); i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "REQ-WMQ-22_uri_table_good_%u", (unsigned)i);
        test_check(broker_cfg_uri_valid(good[i]), nm);
    }
    const char *bad[] = {"MQTT://host", "Mqtts://host", "mqtt://host:00000", "mqtt://host:65535x", "mqtt://host:1883/",
                         "mqtt://host#f", "mqtt://host\n", "mqtt:// host", " mqtt://host", "mqtt://host:+1",
                         "mqtt://host::1883", "mqtt://[::1]:1883", "mqtt://host:1883 ", "mqtt://ho\x80st", "mqtts:///h",
                         "mqtt://host:1234567"};
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "REQ-WMQ-22_uri_table_bad_%u", (unsigned)i);
        test_check(!broker_cfg_uri_valid(bad[i]), nm);
    }

    char unterminated[BROKER_CFG_URI_MAX + 8U];
    memset(unterminated, 'a', sizeof(unterminated));
    memcpy(unterminated, "mqtt://", 7U);
    test_check(!broker_cfg_uri_valid(unterminated), "REQ-WMQ-22_uri_without_nul_within_96_rejected");

    char maxuri[BROKER_CFG_URI_MAX + 1U];
    memcpy(maxuri, "mqtts://", 8U);
    memset(maxuri + 8, 'h', 64U);
    memcpy(maxuri + 72, ":65535", 6U);
    maxuri[78] = '\0';
    test_check(broker_cfg_uri_valid(maxuri) && strlen(maxuri) == 78U, "REQ-WMQ-22_longest_valid_uri_is_78_chars");
}

static void test_nvs_blob_api(void)
{
    broker_cfg_rec_t rec;
    broker_cfg_rec_t out;
    uint8_t buf[sizeof(broker_cfg_rec_t)];
    size_t len;
    esp_err_t ret;

    ret = nvs_config_init();
    test_check(ret == ESP_OK, "REQ-WMQ-23_nvs_init_ok");
    if (ret != ESP_OK) {
        return;
    }

    test_check(nvs_config_erase_key(NVS_KEY_MQTT_CFG) == ESP_OK, "REQ-WMQ-23_erase_absent_key_is_ok");
    len = sizeof(buf);
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, buf, &len) == ESP_ERR_NVS_NOT_FOUND,
               "REQ-WMQ-21_absent_blob_reports_not_found");
    test_check(broker_cfg_decode(buf, 0U, &out) == BROKER_CFG_SRC_FALLBACK_ABSENT,
               "REQ-WMQ-21_absent_blob_decodes_as_absent");

    test_check(broker_cfg_encode(&rec, "mqtts://nvs.lan:8883", "dev_nvs", "pw_nvs"), "REQ-WMQ-23_nvs_record_encodes");
    test_check(nvs_config_set_blob(NVS_KEY_MQTT_CFG, &rec, sizeof(rec)) == ESP_OK, "REQ-WMQ-23_single_blob_write_ok");

    len = 0U;
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, NULL, &len) == ESP_OK && len == sizeof(rec),
               "REQ-WMQ-23_size_probe_reports_record_size");
    len = 16U;
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, buf, &len) == ESP_ERR_NVS_INVALID_LENGTH,
               "REQ-WMQ-23_short_buffer_reports_invalid_length");
    len = sizeof(buf);
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, buf, &len) == ESP_OK && len == sizeof(rec),
               "REQ-WMQ-23_full_read_ok");
    test_check(broker_cfg_decode(buf, len, &out) == BROKER_CFG_SRC_NVS && strcmp(out.uri, "mqtts://nvs.lan:8883") == 0 &&
                   strcmp(out.user, "dev_nvs") == 0 && strcmp(out.pass, "pw_nvs") == 0,
               "REQ-WMQ-21_nvs_roundtrip_is_nvs_source");

    test_check(broker_cfg_encode(&rec, "", "", ""), "REQ-WMQ-21_empty_record_encodes");
    test_check(nvs_config_set_blob(NVS_KEY_MQTT_CFG, &rec, sizeof(rec)) == ESP_OK, "REQ-WMQ-21_overwrite_ok");
    len = sizeof(buf);
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, buf, &len) == ESP_OK &&
                   broker_cfg_decode(buf, len, &out) == BROKER_CFG_SRC_KCONFIG,
               "REQ-WMQ-21_empty_uri_in_nvs_means_kconfig");

    test_check(nvs_config_set_blob(NVS_KEY_MQTT_CFG, &rec, 0U) == ESP_ERR_INVALID_ARG, "REQ-WMQ-23_zero_len_write_rejected");
    test_check(nvs_config_set_blob(NVS_KEY_MQTT_CFG, NULL, sizeof(rec)) == ESP_ERR_INVALID_ARG,
               "REQ-WMQ-23_null_write_rejected");
    test_check(nvs_config_set_blob(NVS_KEY_MQTT_CFG, &rec, NVS_CONFIG_BLOB_MAX + 1U) == ESP_ERR_INVALID_ARG,
               "REQ-WMQ-23_oversize_write_rejected");
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, NULL, NULL) == ESP_ERR_INVALID_ARG, "REQ-WMQ-23_null_len_rejected");
    len = 4U;
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, NULL, &len) == ESP_ERR_INVALID_ARG,
               "REQ-WMQ-23_null_buf_with_len_rejected");
    test_check(nvs_config_set_by_key(NVS_KEY_MQTT_CFG, "mqtt://x") == ESP_ERR_NOT_SUPPORTED,
               "REQ-WMQ-23_blob_not_settable_from_string");

    test_check(nvs_config_erase_key(NVS_KEY_MQTT_CFG) == ESP_OK, "REQ-WMQ-23_erase_key_ok");
    len = sizeof(buf);
    test_check(nvs_config_get_blob(NVS_KEY_MQTT_CFG, buf, &len) == ESP_ERR_NVS_NOT_FOUND,
               "REQ-WMQ-23_erased_key_absent");
    test_check(nvs_config_erase_key(NULL) == ESP_ERR_INVALID_ARG, "REQ-WMQ-23_erase_null_rejected");
    broker_cfg_wipe(&rec);
    broker_cfg_wipe(&out);
}

void test_broker_cfg_run(void)
{
    test_crc_and_layout();
    test_power_loss();
    test_fields();
    test_uri_table();
    test_nvs_blob_api();
}
