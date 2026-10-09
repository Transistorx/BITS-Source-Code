#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Bounded FIFO dispense-job queue.
 *
 * Besides the QUEUED entries, each finished job's record is RETAINED so the
 * API and the log can report what actually happened (target / start / final /
 * error). A retained record is the slot a later add() reuses, so the store
 * stays a fixed, statically allocated array — no heap churn on a device that
 * must never fail an allocation in the middle of a dispense.
 * ------------------------------------------------------------------------ */
#ifndef CONFIG_WEIGHT_DEMO_JOB_QUEUE_MAX
#define CONFIG_WEIGHT_DEMO_JOB_QUEUE_MAX 8
#endif
#define JOB_QUEUE_MAX CONFIG_WEIGHT_DEMO_JOB_QUEUE_MAX

/* Longest error text stored on a job record. */
#define JOB_ERROR_MAX 24U

typedef enum {
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_COMPLETE,
    JOB_CANCELLED,
    JOB_FAILED
} job_state_t;

/* Immutable PID gains pinned to one job.
 *
 * The server ships the exact tuning version the operator chose with every JOB
 * command. Two queued jobs that share a target_g can pin different versions
 * (v2 of tune-a and v3 of tune-a, say), and the device's four-target profile
 * table cannot hold both — so the gains travel with the job rather than living
 * in a shared slot. When `valid` is set the job runs with exactly these
 * numbers and the channel's per-target slot is not consulted. When `valid` is
 * false the job falls back to that slot (the legacy, pin-less path).
 *
 * The staged-dispense extras are part of the pin even though the server does
 * not send them: the firmware fills them from its own Kconfig defaults so a
 * pinned job's COARSE/FINE/MICRO/SETTLING behaviour is fixed at enqueue time
 * too, not re-derived from whatever is staged later. */
typedef struct {
    bool     valid;
    char     profile_id[65];
    uint32_t version;
    float    kp, ki, kd;
    int32_t  tolerance_g, max_overshoot_g;
    uint32_t max_duration_ms, window_ms, min_on_ms, min_off_ms;
    int32_t  coarse_threshold_g, fine_threshold_g, micro_threshold_g;
    uint32_t coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms;
    uint32_t settle_time_ms;
    int32_t  inflight_comp_g;
    /* OPTIONAL NO_PROGRESS thresholds per stage (index 0 COARSE, 1 FINE, 2 MICRO),
     * counted in relay-ON time (CONTRACT 9.11). 0 = absent: the controller then uses
     * its learned bound. UNVALIDATED: no default flow rate is assumed anywhere. */
    uint32_t np_window_ms[3];
    int32_t  np_min_rise_g[3];
} job_profile_t;

/* Queue policy. The running job is NON-PREEMPTIVE: adding a priority-1 job
 * never interrupts whatever is already running. Among WAITING jobs, every
 * priority-1 job is started before any priority-0 job, and within one priority
 * level the order is plain FIFO by insertion. */
#define JOB_PRIORITY_NORMAL 0U   /* 0 - Normal Queue (FIFO) */
#define JOB_PRIORITY_HIGH   1U   /* 1 - Priority */
#define JOB_PRIORITY_MAX    JOB_PRIORITY_HIGH

typedef struct {
    uint32_t id;           /* monotonic, starts at 1; 0 means "no job" */
    uint32_t remote_command_id; /* idempotency key from WSL command queue */
    uint32_t seq;          /* insertion order; snapshot sorts by this */
    uint8_t  material_id;   /* fixed: 1 -> CH1, 2 -> CH2 */
    uint8_t  channel_id;    /* physical scale/relay channel, 1 or 2 */
    uint8_t  priority;     /* JOB_PRIORITY_NORMAL / JOB_PRIORITY_HIGH */
    int32_t  target_g;     /* requested target, integer grams */
    job_state_t state;
    uint32_t requested_ms;
    uint32_t start_ms;
    uint32_t finish_ms;
    int32_t  start_g;      /* weight when the job began (0 if unknown) */
    int32_t  final_g;      /* weight recorded when the job ended */
    /* Operator queue controls. A held job stays QUEUED but is never started;
     * a promoted job is started before any non-promoted job on its channel.
     * Both apply only to a waiting job — never to one already RUNNING. */
    bool     held;
    bool     promoted;
    char     error[JOB_ERROR_MAX];
    /* Exact tuning this job must run with, when the server pinned one. */
    job_profile_t pin;
} dispense_job_t;

const char *job_state_name(job_state_t state);

/* Pure: the accepted target window. An out-of-window request is refused at
 * add() time rather than becoming a job that can never complete. */
bool job_target_valid(int32_t target_g);

/* Pure: is this a known priority level? */
bool job_priority_valid(uint8_t priority);
bool job_material_channel_valid(uint8_t material_id, uint8_t channel_id);

/* Pure: which of two WAITING jobs should start first? Priority first, then
 * insertion order. Exposed so the ordering rule is unit-testable on its own. */
bool job_precedes(uint8_t prio_a, uint32_t seq_a, uint8_t prio_b, uint32_t seq_b);

esp_err_t job_queue_init(void);

/* Appends a QUEUED job. ESP_ERR_INVALID_ARG when the target is out of window,
 * ESP_ERR_NO_MEM when every slot is taken by a queued or running job. */
esp_err_t job_queue_add(int32_t target_g, uint8_t priority, uint32_t now_ms,
                        uint32_t *out_id);
esp_err_t job_queue_add_for_channel(uint8_t channel_id, int32_t target_g,
                                    uint8_t priority, uint32_t now_ms,
                                    uint32_t *out_id);
/* Global jobs have no assigned channel until an idle controller claims the
 * next eligible job. */
esp_err_t job_queue_add_global(int32_t target_g, uint8_t priority,
                               uint32_t now_ms, uint32_t *out_id);
esp_err_t job_queue_add_global_material(uint8_t material_id, int32_t target_g,
                               uint8_t priority, uint32_t now_ms, uint32_t *out_id);
esp_err_t job_queue_add_global_command(int32_t target_g, uint8_t priority,
    uint32_t now_ms, uint32_t command_id, uint32_t *out_id);
esp_err_t job_queue_add_global_material_command(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t now_ms,
    uint32_t command_id, uint32_t *out_id);
/* Same, but the job carries its own immutable PID gains. `pin` may be NULL
 * (legacy / no pin) — the job then falls back to the channel's target slot at
 * start time. A non-NULL pin is copied into the job record and is what runs. */
esp_err_t job_queue_add_global_material_command_pinned(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t now_ms,
    uint32_t command_id, const job_profile_t *pin, uint32_t *out_id);

/* Number of QUEUED (not yet started) jobs — the FIFO depth. */
uint32_t job_queue_depth(void);
uint32_t job_queue_depth_for_channel(uint8_t channel_id);

/* Pops the next QUEUED job and moves it to RUNNING: highest priority first,
 * then oldest-first within that priority. False when none are waiting. */
bool job_queue_start_next(uint32_t now_ms, dispense_job_t *out);
bool job_queue_start_next_channel(uint8_t channel_id, uint32_t now_ms,
                                  dispense_job_t *out);
bool job_queue_start_next_available(uint8_t channel_id, uint32_t now_ms,
                                    dispense_job_t *out);

/* Records the weight the vessel held when the job started. Separate from
 * start_next() because the only component that knows the weight is the
 * dispense controller, which deliberately does not write into the queue's
 * storage directly. */
bool job_queue_set_start_g(uint32_t id, int32_t start_g);

/* Terminal transition for a RUNNING job. Records finish_ms/final_g/error. */
bool job_queue_finish(uint32_t id, job_state_t state, int32_t final_g,
                      const char *error, uint32_t now_ms);

/* Cancels a QUEUED job (a RUNNING job is stopped by the controller). */
bool job_queue_cancel(uint32_t id);

/* Safe un-start: puts a RUNNING job back to QUEUED, keeping its id, insertion order
 * and priority. ONLY for a job the controller claimed but never started (it was
 * waiting for the scale to be moved; no relay was ever turned on). `hold` parks it
 * (held) so the automatic queue does not claim it again at once. False for an
 * unknown id or a job that is not RUNNING. */
bool job_queue_unstart(uint32_t id, bool hold);

/* Operator queue controls. Each applies only to a QUEUED job and refuses a
 * RUNNING one: the queue is non-preemptive and these never touch the valves.
 *
 *   hold     - park the job. It stays in the list and in the depth count but
 *              is never selected by start_next*. Reversible.
 *   release  - undo hold. The job re-enters the normal priority/FIFO order.
 *   promote  - move the job ahead of every non-promoted job on its channel.
 *              Only reorders WAITING jobs; a job already dispensing is
 *              unaffected. A held job can be promoted, and will jump the line
 *              when it is later released.
 *
 * All three return false for an unknown id, a job that is not QUEUED, or when
 * the queue is not initialised. */
bool job_queue_hold(uint32_t id);
bool job_queue_release(uint32_t id);
bool job_queue_promote(uint32_t id);

/* Cancels every QUEUED job; returns how many were cancelled. Emergency-stop
 * path only. There is deliberately no per-channel bulk cancel: removing one
 * job is always addressed by its unique id (job_queue_cancel /
 * dual_dispense_controller_cancel_job) so siblings with a duplicate target_g
 * survive. */
uint32_t job_queue_cancel_all(void);

/* Copies the RUNNING job into `out`. Returns false when none is running.
 *
 * By VALUE, deliberately: an earlier version returned a pointer into a shared
 * static snapshot, so a second caller could overwrite the first caller's data
 * mid-use. The copy is taken under the queue's own mutex. */
bool job_queue_get_running(dispense_job_t *out);

/* Number of QUEUED jobs at a given priority — the web UI shows the split. */
uint32_t job_queue_depth_at(uint8_t priority);

bool job_queue_get(uint32_t id, dispense_job_t *out);

/* Small view of one record (no pin/profile): lets a stack-tight caller look a
 * job up by its server command id without copying a whole dispense_job_t. */
typedef struct {
    uint32_t    id;
    uint8_t     channel_id;
    job_state_t state;
} job_view_t;

/* Finds the record whose remote_command_id matches (0 never matches). Copies
 * the view under the queue mutex; false when unknown or not initialised. */
bool job_queue_find_by_remote(uint32_t remote_command_id, job_view_t *out);

/* Copies up to `max` records, oldest first (by insertion order). Returns the
 * number copied. */
uint32_t job_queue_snapshot(dispense_job_t *out, uint32_t max);

/* Clears every record. For the emergency-stop "cancel everything" path and
 * for tests. */
void job_queue_reset(void);

#endif /* JOB_QUEUE_H */
