#ifndef SCALE_CMD_H
#define SCALE_CMD_H

/*
 * Remote scale commands (ZERO / TARE) delivered over MQTT.
 *
 * STATUS: the CAS CI-150A remote ZERO/TARE frame is UNVERIFIED. No repository
 * document or captured frame contains those bytes (ZERO/TARE are physical keys
 * on the indicator). scale_command_encode() therefore returns
 * SCALE_ENC_NOT_VERIFIED and every accepted command ends as a FAILED ACK with
 * SCALE_CMD_REASON_UNVERIFIED. Bytes must NEVER be guessed: a wrong frame on
 * the scale's RS-485 line could change its configuration or tare state.
 *
 * Pure logic (no esp-mqtt, no UART): everything the outside world provides is
 * injected through scale_cmd_cfg_t so the QEMU suite drives it with mocks.
 * The server and browser are not in any control loop; this is operator intent
 * that firmware validates and may refuse.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scale_cmd_guard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCALE_CMD_REASON_UNVERIFIED \
    "VERIFICATION REQUIRED: CAS CI-150A remote ZERO/TARE frame unverified"

/* CONTRACT 8.4: no path may ACK APPLIED for ZERO/TARE until a verified CI-150A
 * frame exists. Leave 0 until scale_command_encode() carries real, golden-tested
 * bytes; while 0 the executor never transmits and always ACKs FAILED. */
#ifndef SCALE_CMD_ENCODER_VERIFIED
#define SCALE_CMD_ENCODER_VERIFIED 0
#endif

#ifdef SCALE_CMD_TEST_HOOKS
/* TEST ONLY: lets the QEMU suite run the post-encode path with a fake encoder.
 * Compiled only when the test project defines SCALE_CMD_TEST_HOOKS; absent from
 * production. Default off. An APPLIED ack produced this way proves internal
 * logic only, never that the scale accepted a real ZERO/TARE. */
void scale_cmd_test_allow_post_encode(bool on);
#endif

#define SCALE_CMD_REASON_MAX     120U
#define SCALE_CMD_QUEUE_DEPTH    4U
#define SCALE_CMD_DEDUPE_SLOTS   16U
#define SCALE_CMD_TTL_MAX_MS     60000U
#define SCALE_CMD_FRAME_MAX      16U

typedef enum {
    SCALE_CMD_ZERO = 1,
    SCALE_CMD_TARE = 2
} scale_cmd_type_t;

typedef enum {
    SCALE_RESULT_ACCEPTED = 0,  /* reserved: no interim ACK is emitted today */
    SCALE_RESULT_SUCCESS,
    SCALE_RESULT_FAILED,
    SCALE_RESULT_TIMEOUT,
    SCALE_RESULT_REJECTED
} scale_cmd_result_t;

typedef enum {
    SCALE_ENC_OK = 0,
    SCALE_ENC_NOT_VERIFIED
} scale_enc_status_t;

typedef scale_enc_status_t (*scale_cmd_encode_fn)(scale_cmd_type_t type, uint8_t channel,
                                                   uint8_t *frame, size_t cap, size_t *len);

/*
 * CAS CI-150A remote ZERO/TARE encoder. STUB: always NOT_VERIFIED, *len = 0.
 * Replace only with bytes taken from the CAS CI-150A manual or a captured
 * frame, and add a golden-vector test next to the real encoding.
 */
scale_enc_status_t scale_command_encode(scale_cmd_type_t type, uint8_t channel,
                                        uint8_t *frame, size_t cap, size_t *len);

/* Everything an ACK carries. command_id 0 never appears: it is not accepted. */
typedef struct {
    uint32_t command_id;
    bool applied;                         /* state APPLIED (true) or FAILED */
    scale_cmd_result_t result;
    char reason[SCALE_CMD_REASON_MAX + 1U];
    uint8_t channel;                      /* 1 or 2; 0 = unknown/invalid */
    scale_cmd_type_t type;                /* set on SCALE_SUBMIT_QUEUED only, for logging */
    bool has_weight;                     /* only a valid post-command sample */
    int32_t weight_g;
    bool stable;
} scale_cmd_ack_t;

typedef struct {
    uint32_t (*now_ms)(void);
    /* True only when the scale link is ONLINE; *state_name describes why not. */
    bool (*link_online)(const char **state_name);
    /* Latest valid fresh sample of a channel (1/2). seq changes per new frame. */
    bool (*post_sample)(uint8_t channel, uint32_t *seq, int32_t *weight_g, bool *stable);
    /* Writes a verified command frame to the scale transport. May be NULL: with
     * the encoder unverified it is never reached. */
    bool (*send_frame)(uint8_t channel, const uint8_t *frame, size_t len);
    void (*delay_ms)(uint32_t ms);
    scale_cmd_encode_fn encode;           /* NULL = scale_command_encode */
    uint8_t active_channel;               /* 1-based channel this device serves */
    uint32_t settle_timeout_ms;           /* wait for the post-command sample */
    char device_id[65];
    char boot_id[9];                      /* "" = omit from the ACK */
} scale_cmd_cfg_t;

typedef struct {
    uint32_t id;
    scale_cmd_type_t type;
    uint8_t channel;
    uint32_t ttl_ms;
    uint32_t recv_ms;
} scale_cmd_pending_t;

typedef struct {
    uint32_t id;
    uint32_t stamp;                       /* insertion order; 0 = slot empty */
    bool done;                            /* final ACK stored */
    scale_cmd_ack_t ack;
} scale_cmd_slot_t;

typedef struct {
    scale_cmd_cfg_t cfg;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wake;
    scale_cmd_pending_t queue[SCALE_CMD_QUEUE_DEPTH];
    size_t q_head;
    size_t q_count;
    bool running;
    scale_cmd_slot_t slots[SCALE_CMD_DEDUPE_SLOTS];
    uint32_t next_stamp;
} scale_cmd_t;

typedef enum {
    SCALE_SUBMIT_IGNORED = 0,   /* nothing to publish (retained, malformed, in-flight dup) */
    SCALE_SUBMIT_QUEUED,        /* accepted; the final ACK comes from run_one */
    SCALE_SUBMIT_ACK            /* *ack must be published now (reject or duplicate) */
} scale_submit_t;

bool scale_cmd_init(scale_cmd_t *s, const scale_cmd_cfg_t *cfg);

/* Validates, dedupes and queues one MQTT command payload. Never blocks on the
 * scale and never touches the transport. */
scale_submit_t scale_cmd_submit(scale_cmd_t *s, const char *payload, size_t len,
                                bool retain, uint32_t recv_ms, scale_cmd_ack_t *ack);

/* The scale_cmd task body: waits up to wait_ms for a queued command, executes
 * it (the only place that may touch the scale transport) and fills *ack. */
bool scale_cmd_run_one(scale_cmd_t *s, uint32_t wait_ms, scale_cmd_ack_t *ack);

bool scale_cmd_busy(scale_cmd_t *s);

size_t scale_cmd_build_ack_json(const scale_cmd_t *s, const scale_cmd_ack_t *ack,
                                char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SCALE_CMD_H */
