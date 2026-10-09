#ifndef TELEMETRY_CLIENT_H
#define TELEMETRY_CLIENT_H

#include "dual_dispense_controller.h"
#include "esp_err.h"
#include "job_queue.h"
#include "weight_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts separate history, command and live-status tasks (plus the mqtt_cmd
 * task when an MQTT transport was registered). Network I/O never runs in the
 * scale callback or supervisor task; commands call the serialized controller
 * API after the HTTP response (or MQTT message) is received. */
esp_err_t telemetry_client_start(void);
const char *telemetry_client_device_id(void);

/* ---- MQTT transport hooks ------------------------------------------------
 * telemetry_client does not link esp-mqtt: main wires mqtt_link in here before
 * telemetry_client_start(). With no ops registered everything stays HTTP. */
#define TELEMETRY_MQTT_RX_MAX 1024U
typedef struct {
    uint16_t len;
    uint32_t recv_ms;   /* tick-based ms at receipt (command ttl domain) */
    int64_t  recv_us;   /* esp_timer at receipt (latency measurement) */
    char     data[TELEMETRY_MQTT_RX_MAX + 1U];
} telemetry_mqtt_rx_t;

typedef struct {
    bool (*connected)(void);
    /* Zero-timeout enqueue under cas/{device}/{suffix}; false = not queued. */
    bool (*publish)(const char *suffix, const char *json, int qos, bool retain);
    /* Next inbound command message, waiting up to timeout_ms. */
    bool (*rx_pop)(telemetry_mqtt_rx_t *out, uint32_t timeout_ms);
    /* Optional (run_mqtt): next run/ack body from the link's separate ack queue. Only the
     * run_mqtt task calls it; the command RX path never sees an ack. */
    bool (*run_ack_pop)(char *out, size_t cap, size_t *len, uint32_t timeout_ms);
} telemetry_mqtt_ops_t;
void telemetry_client_set_mqtt_ops(const telemetry_mqtt_ops_t *ops);

/* ---- Command dispatch (ONE path for HTTP poll and MQTT) ------------------ */
typedef enum { TELEMETRY_CMD_HTTP = 0, TELEMETRY_CMD_MQTT = 1 } telemetry_cmd_transport_t;

/* ACK sink. state is QUEUED|APPLIED|FAILED. `error` is never NULL. */
typedef void (*telemetry_cmd_ack_fn)(void *ctx, uint32_t command_id, const char *state,
                                     uint32_t local_job_id, uint8_t channel, const char *error);

/* Run one command object. Dedupes by command_id across both transports (a
 * duplicate is re-ACKed with the stored outcome, never re-run; PUMP_STOP is the
 * exception: it is idempotent and always re-applied). Enforces ttl_ms against
 * recv_ms. `now_ms` is the controller time base. Exposed for QEMU. */
void telemetry_client_dispatch_command(const char *object, telemetry_cmd_transport_t via,
                                       uint32_t recv_ms, uint32_t now_ms,
                                       telemetry_cmd_ack_fn ack, void *ctx);

/* Walk one HTTP commands response (STOP-class first, then the rest) and
 * dispatch each object. now_ms == TELEMETRY_NOW_LIVE reads the tick clock per
 * command (production); tests pass a fixed time. Exposed for QEMU. */
#define TELEMETRY_NOW_LIVE UINT32_MAX
void telemetry_client_http_process_response(const char *response, uint32_t recv_ms,
                                            uint32_t now_ms, telemetry_cmd_ack_fn ack, void *ctx);

typedef enum {
    TELEMETRY_INTAKE_URGENT = 0,   /* STOP-class: ESTOP / PUMP_STOP */
    TELEMETRY_INTAKE_NORMAL,
    TELEMETRY_INTAKE_RETAINED,     /* retain=1: ignored */
    TELEMETRY_INTAKE_INVALID,      /* empty, oversize, no command_id */
    TELEMETRY_INTAKE_FULL          /* staging full: dropped, HTTP poll re-serves it */
} telemetry_intake_result_t;

/* mqtt_cmd task side: classify one inbound message into the urgent or normal
 * staging queue (zero timeout, no blocking). */
telemetry_intake_result_t telemetry_client_mqtt_intake(const char *payload, size_t len,
                                                       bool retain, uint32_t recv_ms,
                                                       int64_t recv_us);
/* Executes every staged STOP-class command, then ONE normal command (the task
 * drains the RX queue between normal commands so a STOP is never stuck behind
 * more than one). ACKs go out on MQTT. Returns true when something ran. */
bool telemetry_client_mqtt_step(uint32_t now_ms);
size_t telemetry_client_mqtt_staged(void);
/* mqtt_cmd task body: pops up to 16 messages from ops.rx_pop (first wait first_timeout_ms,
 * then zero) into the intake queues; returns how many were taken. Exposed for QEMU. */
size_t telemetry_client_mqtt_drain(uint32_t first_timeout_ms);

typedef struct {
    uint32_t intake_urgent, intake_normal;
    uint32_t drop_retained, drop_invalid, drop_full_urgent, drop_full_normal;
    uint32_t cmd_executed, dup_reacked, dup_inflight, expired, deferred_to_http;
    uint32_t urgent_executed, normal_executed;
    uint32_t manual_start_exec, manual_stop_exec;
    uint32_t lat_urgent_last_us, lat_urgent_max_us;  /* receipt -> start of dispatch */
    uint32_t lat_normal_last_us, lat_normal_max_us;
    uint32_t stale_refused;   /* non-STOP id at/below the evicted-ledger floor */
} telemetry_cmd_stats_t;
void telemetry_client_cmd_stats(telemetry_cmd_stats_t *out);
/* Test-only: forget processed command ids, staged commands and counters. */
void telemetry_client_test_reset_commands(void);
void telemetry_client_on_weight(const weight_msg_t *weight, uint32_t received_ms);
/* Command task owns this bounded cancellation ledger. Exposed for QEMU. */
bool telemetry_client_note_remote_cancel(uint32_t command_id);
bool telemetry_client_remote_job_cancelled(uint32_t command_id);

/* READY device command (CONTRACT 9.11): the operator confirms the scale was moved to
 * this pump. Returns true (ACK APPLIED) when it released a channel waiting for the
 * scale move, or when the command_id was already executed. Returns false with *error
 * set (a stable READY_* code first) otherwise; a refused READY changes nothing and
 * never touches a relay. The legacy form carries only {channel}. Exposed for QEMU. */
bool telemetry_client_handle_ready(uint32_t command_id, uint8_t channel,
                                   uint32_t now_ms, const char **error);

typedef struct {
    bool     has_job_id, has_boot;   /* both present: new form; neither: legacy; one: refused */
    bool     malformed;              /* a present field failed to parse */
    uint32_t job_id;                 /* the device's own job id (awaiting_scale_move.job_id) */
    char     scale_boot_id[9];       /* the live sender boot_id (status scale_boot_id) */
    uint32_t age_ms;                 /* time since the READY was received */
} telemetry_ready_t;
bool telemetry_client_handle_ready_ex(uint32_t command_id, uint8_t channel,
                                      const telemetry_ready_t *ready, uint32_t now_ms,
                                      const char **error);

/* Build the MQTT telemetry/live body for CH1,CH2 (snap[0], snap[1]). Invalid weight
 * -> "weight_g":null; UINT32_MAX age -> null. Returns length or -1 if it does not
 * fit in cap (caller must then not publish). Exposed for QEMU. */
int telemetry_client_live_body(char *buf, size_t cap, uint32_t now,
                               const dual_channel_snapshot_t snap[2]);

/* The relay status object exactly as upload_device_status() publishes it (CONTRACT 7/9.11).
 * Returns its length, or -1 if it does not fit in cap. Exposed for QEMU. */
int telemetry_client_status_json(char *buf, size_t cap, uint32_t now_ms);

/* Test-only: clear READY/manual dedup state so tests do not depend on id order. */
void telemetry_client_test_reset_ready(void);

/* Test-only seam for the HTTP response accumulator (one body chunk per call).
 * Returns false once the response has overflowed `cap`. */
bool telemetry_client_test_response_append(char *buf, size_t cap, bool *truncated,
                                           const char *data, size_t len);

/* Outcome of reading a JOB command's pinned PID profile. */
typedef enum {
    JOB_PIN_REFUSED = -1, /* pin present but incomplete/invalid -> refuse the job */
    JOB_PIN_ABSENT  = 0,  /* no pin on the command -> legacy fetch_profile path */
    JOB_PIN_PARSED  = 1,  /* pin present and copied into *out (out->valid = true) */
} job_pin_result_t;

/* Parse the pinned profile the server ships with a JOB command.
 *
 * The server pins the exact immutable tuning version the operator chose and
 * sends its gains alongside the job, so the device never substitutes "whatever
 * is currently active for this target". A pin that is present but unusable
 * (gains missing because the row was deleted after queueing, no version, …)
 * is a REFUSED — never a silent fallback to the shared slot.
 *
 * `command_object` is one command's JSON object. `error`/`error_cap` receive a
 * short operator-readable reason on REFUSED (may be NULL). Exposed for QEMU
 * tests; production callers are the JOB handler in telemetry_client.c. */
job_pin_result_t telemetry_client_parse_job_pin(const char *command_object,
                                                uint8_t material_id,
                                                int32_t target_g,
                                                job_profile_t *out,
                                                char *error, size_t error_cap);

/* ---- CONTRACT 9.2 / 9.3 (both flags default OFF; NVS run_mqtt, req_pin_prof) ----
 * run_mqtt: run/start|samples|events|complete go out over MQTT QoS 1 from a RAM
 * ring (run_log.h) and the HTTP run upload is not used. require_pinned_profile:
 * a JOB without a pinned profile is refused (PROFILE_REQUIRED), never defaulted. */
bool telemetry_client_run_mqtt_enabled(void);
bool telemetry_client_require_pin(void);
/* cas/{dev}/run/ack body, from the run_mqtt task only. Zero-timeout, no I/O. */
void telemetry_client_run_ack(const char *json, size_t len);
/* Applies the acks waiting in ops.run_ack_pop (at most 6). Returns how many. */
size_t telemetry_client_run_ack_drain(void);
/* NVS hash_check (default 0): verify profile_hash and report applied_profile (9.3). */
bool telemetry_client_hash_check_enabled(void);
/* True when run history state was allocated; false = run tracking disabled (counted,
 * logged) while commands, STOP and live status keep working. */
bool telemetry_client_run_tracking_enabled(void);
uint32_t telemetry_client_run_alloc_failures(void);

/* profile_hash: SHA-256 over the canonical compact JSON of the JOB's profile{}
 * (members sorted by key, no whitespace outside strings, value tokens verbatim),
 * first 16 hex into out[17]. False when the command has no well-formed profile{}. */
bool telemetry_client_job_profile_hash(const char *command_object, char out[17]);

/* Test-only seams. */
void telemetry_client_test_set_flags(bool run_mqtt, bool require_pin);
void telemetry_client_test_set_hash_check(bool on);
/* Runs the start-up run-state setup; force_fail simulates an allocation failure.
 * Returns whether run tracking ended up enabled. Clears run_mqtt on failure. */
bool telemetry_client_test_run_tracking_init(bool force_fail);
/* Same for the run MQTT ring; on failure clears run_mqtt (HTTP fallback) and counts it. */
bool telemetry_client_test_run_mqtt_ring_init(bool force_fail);
void telemetry_client_test_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
/* profile recorded for command_id's ACK (what was read back from the queued job). */
bool telemetry_client_test_applied_profile(uint32_t command_id, char *profile_id, size_t cap,
                                           uint32_t *version, char hash[17]);
/* One whole run for `job` on channel ch with n synthetic samples; returns the
 * run's batch_seq before it was closed. Needs run_mqtt on and run_log initialised. */
uint32_t telemetry_client_test_run_cycle(uint8_t ch, const dispense_job_t *job,
                                         const dual_channel_snapshot_t *snap,
                                         uint32_t now, uint32_t n);
/* run_start only (no reset of the slot): a pending un-closed run is superseded, not forgotten. */
void telemetry_client_test_run_begin(uint8_t ch, const dispense_job_t *job,
                                     const dual_channel_snapshot_t *snap, uint32_t now);

#endif
