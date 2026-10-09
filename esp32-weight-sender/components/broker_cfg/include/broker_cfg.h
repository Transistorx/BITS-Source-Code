#ifndef BROKER_CFG_H
#define BROKER_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BROKER_CFG_VERSION 1U
#define BROKER_CFG_MAGIC 0x42524B31U
#define BROKER_CFG_URI_MAX 95U
#define BROKER_CFG_USER_MAX 31U
#define BROKER_CFG_PASS_MAX 63U
#define BROKER_CFG_HOST_MAX 64U
#define BROKER_CFG_PORT_MIN 1U
#define BROKER_CFG_PORT_MAX 65535U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    char uri[BROKER_CFG_URI_MAX + 1U];
    char user[BROKER_CFG_USER_MAX + 1U];
    char pass[BROKER_CFG_PASS_MAX + 1U];
    uint32_t crc32;
} broker_cfg_rec_t;

typedef enum {
    BROKER_CFG_SRC_KCONFIG = 0,
    BROKER_CFG_SRC_NVS,
    BROKER_CFG_SRC_FALLBACK_ABSENT,
    BROKER_CFG_SRC_FALLBACK_VERSION,
    BROKER_CFG_SRC_FALLBACK_CRC,
    BROKER_CFG_SRC_FALLBACK_INVALID
} broker_cfg_src_t;

typedef struct {
    broker_cfg_src_t src;
    const char *uri;
    const char *user;
    const char *pass;
    bool tls;
} broker_cfg_eff_t;

bool broker_cfg_uri_valid(const char *uri);
bool broker_cfg_uri_is_tls(const char *uri);
bool broker_cfg_encode(broker_cfg_rec_t *rec, const char *uri, const char *user, const char *pass);
broker_cfg_src_t broker_cfg_decode(const void *blob, size_t len, broker_cfg_rec_t *out);
void broker_cfg_effective(broker_cfg_src_t src, const broker_cfg_rec_t *rec, const char *kc_uri,
                          const char *kc_user, const char *kc_pass, broker_cfg_eff_t *eff);
const char *broker_cfg_src_name(broker_cfg_src_t src);
uint32_t broker_cfg_crc32(const void *data, size_t len);
void broker_cfg_wipe(broker_cfg_rec_t *rec);
void broker_cfg_wipe_bytes(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
