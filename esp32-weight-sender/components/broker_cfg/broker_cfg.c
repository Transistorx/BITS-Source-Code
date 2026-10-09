#include "broker_cfg.h"

#include <string.h>

#define BROKER_CFG_PORT_DIGITS_MAX 5U
#define BROKER_CFG_CRC_POLY 0xEDB88320U

_Static_assert(offsetof(broker_cfg_rec_t, crc32) == 200U, "broker_cfg_rec_t layout changed");
_Static_assert(sizeof(broker_cfg_rec_t) == 204U, "broker_cfg_rec_t size changed");

static const char *const s_src_names[] = {
    "kconfig", "nvs", "fallback_absent", "fallback_version", "fallback_crc", "fallback_invalid",
};

static void wipe_bytes(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0U; i < n; i++) {
        v[i] = 0U;
    }
}

void broker_cfg_wipe(broker_cfg_rec_t *rec)
{
    if (rec != NULL) {
        wipe_bytes(rec, sizeof(*rec));
    }
}

uint32_t broker_cfg_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFU;
    if (p == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < len; i++) {
        c ^= p[i];
        for (unsigned k = 0U; k < 8U; k++) {
            c = (c >> 1) ^ (BROKER_CFG_CRC_POLY & (0U - (c & 1U)));
        }
    }
    return ~c;
}

const char *broker_cfg_src_name(broker_cfg_src_t src)
{
    size_t idx = (size_t)src;
    if (idx >= (sizeof(s_src_names) / sizeof(s_src_names[0]))) {
        return "unknown";
    }
    return s_src_names[idx];
}

static bool host_char(char c)
{
    return ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
           (c == '.') || (c == '-');
}

bool broker_cfg_uri_valid(const char *uri)
{
    if (uri == NULL) {
        return false;
    }
    size_t total = strnlen(uri, BROKER_CFG_URI_MAX + 1U);
    if ((total == 0U) || (total > BROKER_CFG_URI_MAX)) {
        return false;
    }

    size_t pos;
    if (strncmp(uri, "mqtts://", 8U) == 0) {
        pos = 8U;
    } else if (strncmp(uri, "mqtt://", 7U) == 0) {
        pos = 7U;
    } else {
        return false;
    }

    size_t host_len = 0U;
    while ((uri[pos] != '\0') && host_char(uri[pos])) {
        pos++;
        host_len++;
    }
    if ((host_len == 0U) || (host_len > BROKER_CFG_HOST_MAX)) {
        return false;
    }

    if (uri[pos] == ':') {
        pos++;
        uint32_t port = 0U;
        size_t digits = 0U;
        while ((uri[pos] >= '0') && (uri[pos] <= '9')) {
            if (digits >= BROKER_CFG_PORT_DIGITS_MAX) {
                return false;
            }
            port = (port * 10U) + (uint32_t)(uri[pos] - '0');
            pos++;
            digits++;
        }
        if ((digits == 0U) || (port < BROKER_CFG_PORT_MIN) || (port > BROKER_CFG_PORT_MAX)) {
            return false;
        }
    }

    return uri[pos] == '\0';
}

static bool field_terminated(const char *field, size_t cap)
{
    return memchr(field, 0, cap) != NULL;
}

bool broker_cfg_encode(broker_cfg_rec_t *rec, const char *uri, const char *user, const char *pass)
{
    bool ok = false;

    if (rec == NULL) {
        goto cleanup;
    }
    broker_cfg_wipe(rec);
    if ((uri == NULL) || (user == NULL) || (pass == NULL)) {
        goto cleanup;
    }
    if ((uri[0] != '\0') && !broker_cfg_uri_valid(uri)) {
        goto cleanup;
    }
    if (strnlen(user, BROKER_CFG_USER_MAX + 1U) > BROKER_CFG_USER_MAX) {
        goto cleanup;
    }
    if (strnlen(pass, BROKER_CFG_PASS_MAX + 1U) > BROKER_CFG_PASS_MAX) {
        goto cleanup;
    }

    rec->magic = BROKER_CFG_MAGIC;
    rec->version = (uint16_t)BROKER_CFG_VERSION;
    rec->flags = 0U;
    (void)strncpy(rec->uri, uri, BROKER_CFG_URI_MAX);
    (void)strncpy(rec->user, user, BROKER_CFG_USER_MAX);
    (void)strncpy(rec->pass, pass, BROKER_CFG_PASS_MAX);
    rec->crc32 = broker_cfg_crc32(rec, offsetof(broker_cfg_rec_t, crc32));
    ok = true;

cleanup:
    if (!ok && (rec != NULL)) {
        broker_cfg_wipe(rec);
    }
    return ok;
}

broker_cfg_src_t broker_cfg_decode(const void *blob, size_t len, broker_cfg_rec_t *out)
{
    broker_cfg_rec_t tmp;
    broker_cfg_src_t src = BROKER_CFG_SRC_FALLBACK_INVALID;

    if (out != NULL) {
        broker_cfg_wipe(out);
    }
    if ((blob == NULL) || (len == 0U)) {
        src = BROKER_CFG_SRC_FALLBACK_ABSENT;
        goto cleanup;
    }
    if (out == NULL) {
        goto cleanup;
    }
    if (len != sizeof(tmp)) {
        src = BROKER_CFG_SRC_FALLBACK_VERSION;
        goto cleanup;
    }

    memcpy(&tmp, blob, sizeof(tmp));
    if (broker_cfg_crc32(&tmp, offsetof(broker_cfg_rec_t, crc32)) != tmp.crc32) {
        src = BROKER_CFG_SRC_FALLBACK_CRC;
        goto wipe_tmp;
    }
    if ((tmp.magic != BROKER_CFG_MAGIC) || (tmp.version != (uint16_t)BROKER_CFG_VERSION)) {
        src = BROKER_CFG_SRC_FALLBACK_VERSION;
        goto wipe_tmp;
    }
    if (tmp.flags != 0U) {
        goto wipe_tmp;
    }
    if (!field_terminated(tmp.uri, sizeof(tmp.uri)) || !field_terminated(tmp.user, sizeof(tmp.user)) ||
        !field_terminated(tmp.pass, sizeof(tmp.pass))) {
        goto wipe_tmp;
    }
    if (tmp.uri[0] == '\0') {
        src = BROKER_CFG_SRC_KCONFIG;
        goto wipe_tmp;
    }
    if (!broker_cfg_uri_valid(tmp.uri)) {
        goto wipe_tmp;
    }

    memcpy(out, &tmp, sizeof(tmp));
    src = BROKER_CFG_SRC_NVS;

wipe_tmp:
    broker_cfg_wipe(&tmp);
cleanup:
    return src;
}

bool broker_cfg_uri_is_tls(const char *uri)
{
    static const char prefix[] = "mqtts://";
    if (uri == NULL) {
        return false;
    }
    return strncmp(uri, prefix, sizeof(prefix) - 1U) == 0;
}

void broker_cfg_effective(broker_cfg_src_t src, const broker_cfg_rec_t *rec, const char *kc_uri,
                          const char *kc_user, const char *kc_pass, broker_cfg_eff_t *eff)
{
    static const char empty[] = "";

    if (eff == NULL) {
        return;
    }
    eff->src = src;
    eff->uri = (kc_uri != NULL) ? kc_uri : empty;
    eff->user = (kc_user != NULL) ? kc_user : empty;
    eff->pass = (kc_pass != NULL) ? kc_pass : empty;
    if ((src == BROKER_CFG_SRC_NVS) && (rec != NULL) && (rec->magic == BROKER_CFG_MAGIC) &&
        (rec->version == (uint16_t)BROKER_CFG_VERSION) && (rec->uri[0] != '\0') &&
        (memchr(rec->uri, 0, sizeof(rec->uri)) != NULL) && (memchr(rec->user, 0, sizeof(rec->user)) != NULL) &&
        (memchr(rec->pass, 0, sizeof(rec->pass)) != NULL)) {
        eff->uri = rec->uri;
        eff->user = rec->user;
        eff->pass = rec->pass;
    } else if (src == BROKER_CFG_SRC_NVS) {
        eff->src = BROKER_CFG_SRC_FALLBACK_INVALID;
    }
    eff->tls = broker_cfg_uri_is_tls(eff->uri);
}
