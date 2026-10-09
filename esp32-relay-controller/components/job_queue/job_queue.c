#include "job_queue.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "RELAY_CTRL";

#ifndef CONFIG_WEIGHT_DEMO_MAX_TARGET_G
#define CONFIG_WEIGHT_DEMO_MAX_TARGET_G 20000
#endif
#define JOB_MAX_TARGET_G CONFIG_WEIGHT_DEMO_MAX_TARGET_G

/* A target of zero is a no-op job: it would "complete" instantly without
 * dispensing anything, which makes a mis-typed request look like a success. */
#define JOB_MIN_TARGET_G 1

static dispense_job_t s_slots[JOB_QUEUE_MAX];
static uint32_t s_next_id;
static uint32_t s_next_seq;

/* The queue is driven by the dispense controller (supervisor task) and read
 * and written by the web API (httpd task). A mutex — not a spinlock: the
 * sections touch only memory, but they are long enough that a spinlock would
 * burn a whole time slice, and no caller runs in an ISR. */
static SemaphoreHandle_t s_lock;

const char *job_state_name(job_state_t state)
{
    switch (state) {
    case JOB_QUEUED:    return "QUEUED";
    case JOB_RUNNING:   return "RUNNING";
    case JOB_COMPLETE:  return "COMPLETE";
    case JOB_CANCELLED: return "CANCELLED";
    case JOB_FAILED:    return "FAILED";
    default:            return "UNKNOWN";
    }
}

bool job_target_valid(int32_t target_g)
{
    return target_g >= JOB_MIN_TARGET_G && target_g <= JOB_MAX_TARGET_G;
}

bool job_priority_valid(uint8_t priority)
{
    return priority <= JOB_PRIORITY_MAX;
}

bool job_material_channel_valid(uint8_t material_id, uint8_t channel_id)
{
    return (material_id == 1U || material_id == 2U) && material_id == channel_id;
}

bool job_precedes(uint8_t prio_a, uint32_t seq_a, uint8_t prio_b, uint32_t seq_b)
{
    /* Higher priority wins outright; within one priority the earlier insertion
     * wins. Strictly ordered, so `a precedes b` and `b precedes a` can never
     * both be true and the selection is deterministic. */
    if (prio_a != prio_b) return prio_a > prio_b;
    return seq_a < seq_b;
}

/* Caller holds the lock. Whether a WAITING job `a` should start before `b`:
 * a promoted job beats any non-promoted one regardless of priority, then the
 * ordinary priority-then-FIFO rule applies. Strictly ordered for the same
 * reason job_precedes() is. */
static bool job_outweighs(const dispense_job_t *a, const dispense_job_t *b)
{
    if (a->promoted != b->promoted) return a->promoted;
    return job_precedes(a->priority, a->seq, b->priority, b->seq);
}

esp_err_t job_queue_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1U;
    s_next_seq = 1U;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Caller holds the lock. A slot is reusable when it is empty (id 0) or holds a
 * job that already reached a terminal state. */
static dispense_job_t *find_free_slot(void)
{
    dispense_job_t *fallback = NULL;

    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id == 0U) return &s_slots[i];
        if (s_slots[i].state == JOB_COMPLETE ||
            s_slots[i].state == JOB_CANCELLED ||
            s_slots[i].state == JOB_FAILED) {
            /* Reuse the OLDEST finished record first, so recent history
             * survives a burst of new requests. */
            if (fallback == NULL || s_slots[i].seq < fallback->seq) {
                fallback = &s_slots[i];
            }
        }
    }
    return fallback;
}

esp_err_t job_queue_add(int32_t target_g, uint8_t priority, uint32_t now_ms,
                        uint32_t *out_id)
{
    return job_queue_add_for_channel(1U, target_g, priority, now_ms, out_id);
}

esp_err_t job_queue_add_for_channel(uint8_t channel_id, int32_t target_g,
                                    uint8_t priority, uint32_t now_ms,
                                    uint32_t *out_id)
{
    if (channel_id < 1U || channel_id > 2U) return ESP_ERR_INVALID_ARG;
    if (!job_target_valid(target_g)) return ESP_ERR_INVALID_ARG;
    if (!job_priority_valid(priority)) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    uint32_t id = 0U;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dispense_job_t *slot = find_free_slot();
    if (slot == NULL) {
        /* Every slot is QUEUED or RUNNING: the queue really is full. */
        err = ESP_ERR_NO_MEM;
    } else {
        memset(slot, 0, sizeof(*slot));
        slot->id = s_next_id++;
        slot->seq = s_next_seq++;
        slot->channel_id = channel_id;
        slot->material_id = channel_id;
        slot->target_g = target_g;
        slot->priority = priority;
        slot->state = JOB_QUEUED;
        slot->requested_ms = now_ms;
        slot->error[0] = '\0';
        id = slot->id;
    }

    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Job %u queued | channel=%u target=%ld g | priority=%u (%s) | depth=%u",
                 (unsigned)id, (unsigned)channel_id, (long)target_g, (unsigned)priority,
                 (priority == JOB_PRIORITY_HIGH) ? "Priority" : "Normal Queue (FIFO)",
                 (unsigned)job_queue_depth());
        if (out_id != NULL) *out_id = id;
    } else {
        ESP_LOGW(TAG, "Job refused (queue full) | target=%ld g priority=%u",
                 (long)target_g, (unsigned)priority);
    }
    return err;
}

esp_err_t job_queue_add_global(int32_t target_g, uint8_t priority,
                               uint32_t now_ms, uint32_t *out_id)
{
    return job_queue_add_global_material(1U, target_g, priority, now_ms, out_id);
}

esp_err_t job_queue_add_global_material(uint8_t material_id, int32_t target_g,
                               uint8_t priority, uint32_t now_ms, uint32_t *out_id)
{
    if ((material_id != 1U && material_id != 2U) || !job_target_valid(target_g) || !job_priority_valid(priority))
        return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    uint32_t id = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dispense_job_t *slot = find_free_slot();
    if (slot == NULL) err = ESP_ERR_NO_MEM;
    else {
        memset(slot, 0, sizeof(*slot));
        slot->id = s_next_id++;
        slot->seq = s_next_seq++;
        slot->channel_id = 0U; /* assigned by the scheduler at start */
        slot->material_id = material_id;
        slot->target_g = target_g;
        slot->priority = priority;
        slot->state = JOB_QUEUED;
        slot->requested_ms = now_ms;
        id = slot->id;
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        if (out_id) *out_id = id;
        ESP_LOGI(TAG, "JOB_QUEUED id=%u material=M%u compatible_channel=CH%u target=%ld g priority=%u (%s) depth=%u",
                 (unsigned)id, (unsigned)material_id, (unsigned)material_id,
                 (long)target_g, (unsigned)priority,
                 priority == JOB_PRIORITY_HIGH ? "Priority" : "Normal FIFO",
                 (unsigned)job_queue_depth());
    }
    return err;
}

esp_err_t job_queue_add_global_command(int32_t target_g, uint8_t priority,
    uint32_t now_ms, uint32_t command_id, uint32_t *out_id)
{
    return job_queue_add_global_material_command(1U, target_g, priority, now_ms,
                                                 command_id, out_id);
}

esp_err_t job_queue_add_global_material_command(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t now_ms, uint32_t command_id,
    uint32_t *out_id)
{
    return job_queue_add_global_material_command_pinned(material_id, target_g,
        priority, now_ms, command_id, NULL, out_id);
}

esp_err_t job_queue_add_global_material_command_pinned(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t now_ms, uint32_t command_id,
    const job_profile_t *pin, uint32_t *out_id)
{
    if (command_id == 0U || s_lock == NULL ||
        (material_id != 1U && material_id != 2U) ||
        !job_target_valid(target_g) || !job_priority_valid(priority)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < JOB_QUEUE_MAX; ++i) {
        if (s_slots[i].id && s_slots[i].remote_command_id == command_id) {
            if (out_id) *out_id = s_slots[i].id;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    uint32_t id = 0U;
    esp_err_t err = job_queue_add_global_material(material_id, target_g, priority, now_ms, &id);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < JOB_QUEUE_MAX; ++i) {
        if (s_slots[i].id == id) {
            s_slots[i].remote_command_id = command_id;
            /* Copy the pin into the job record. Two queued jobs may share a
             * target_g and still pin different versions; the shared per-target
             * profile slot cannot represent both, so the gains travel with the
             * job and are what actually runs. */
            if (pin != NULL) s_slots[i].pin = *pin;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (out_id) *out_id = id;
    return ESP_OK;
}

uint32_t job_queue_depth(void)
{
    if (s_lock == NULL) return 0U;

    uint32_t depth = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U && s_slots[i].state == JOB_QUEUED) depth++;
    }
    xSemaphoreGive(s_lock);
    return depth;
}

uint32_t job_queue_depth_for_channel(uint8_t channel_id)
{
    if (s_lock == NULL || channel_id < 1U || channel_id > 2U) return 0U;
    uint32_t depth = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; ++i)
        if (s_slots[i].id != 0U && s_slots[i].state == JOB_QUEUED &&
            s_slots[i].channel_id == channel_id) ++depth;
    xSemaphoreGive(s_lock);
    return depth;
}

uint32_t job_queue_depth_at(uint8_t priority)
{
    if (s_lock == NULL) return 0U;

    uint32_t depth = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U && s_slots[i].state == JOB_QUEUED &&
            s_slots[i].priority == priority) {
            depth++;
        }
    }
    xSemaphoreGive(s_lock);
    return depth;
}

bool job_queue_start_next(uint32_t now_ms, dispense_job_t *out)
{
    return job_queue_start_next_channel(1U, now_ms, out);
}

bool job_queue_start_next_channel(uint8_t channel_id, uint32_t now_ms,
                                  dispense_job_t *out)
{
    if (s_lock == NULL || channel_id < 1U || channel_id > 2U) return false;

    bool found = false;
    dispense_job_t picked;
    memset(&picked, 0, sizeof(picked));

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Promoted first, then priority, then FIFO within the level. A held job is
     * never eligible. NON-PREEMPTIVE by construction: this only ever looks at
     * QUEUED jobs, so a high-priority or promoted arrival can never disturb
     * the job already RUNNING. */
    dispense_job_t *best = NULL;
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id == 0U || s_slots[i].state != JOB_QUEUED ||
            s_slots[i].held ||
            !job_material_channel_valid(s_slots[i].material_id, channel_id) ||
            s_slots[i].channel_id != channel_id) continue;
        if (best == NULL || job_outweighs(&s_slots[i], best)) {
            best = &s_slots[i];
        }
    }

    if (best != NULL) {
        best->state = JOB_RUNNING;
        best->start_ms = now_ms;
        best->final_g = 0;
        picked = *best;
        found = true;
    }

    xSemaphoreGive(s_lock);

    if (found && out != NULL) *out = picked;
    return found;
}

bool job_queue_start_next_available(uint8_t channel_id, uint32_t now_ms,
                                    dispense_job_t *out)
{
    if (s_lock == NULL || channel_id < 1U || channel_id > 2U) return false;
    bool found = false;
    dispense_job_t picked;
    memset(&picked, 0, sizeof(picked));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dispense_job_t *best = NULL;
    for (uint32_t i = 0; i < JOB_QUEUE_MAX; ++i) {
        if (s_slots[i].id == 0U || s_slots[i].state != JOB_QUEUED ||
            s_slots[i].held ||
            !job_material_channel_valid(s_slots[i].material_id, channel_id) ||
            (s_slots[i].channel_id != 0U && s_slots[i].channel_id != channel_id)) continue;
        if (best == NULL || job_outweighs(&s_slots[i], best)) best = &s_slots[i];
    }
    if (best != NULL) {
        best->channel_id = channel_id;
        best->state = JOB_RUNNING;
        best->start_ms = now_ms;
        best->final_g = 0;
        picked = *best;
        found = true;
    }
    xSemaphoreGive(s_lock);
    if (found && out) *out = picked;
    return found;
}

bool job_queue_set_start_g(uint32_t id, int32_t start_g)
{
    if (s_lock == NULL || id == 0U) return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id == id) {
            s_slots[i].start_g = start_g;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool job_queue_finish(uint32_t id, job_state_t state, int32_t final_g,
                      const char *error, uint32_t now_ms)
{
    if (s_lock == NULL || id == 0U) return false;
    if (state != JOB_COMPLETE && state != JOB_CANCELLED && state != JOB_FAILED) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != id) continue;
        s_slots[i].state = state;
        s_slots[i].finish_ms = now_ms;
        s_slots[i].final_g = final_g;
        if (error != NULL) {
            strncpy(s_slots[i].error, error, JOB_ERROR_MAX - 1U);
            s_slots[i].error[JOB_ERROR_MAX - 1U] = '\0';
        } else {
            s_slots[i].error[0] = '\0';
        }
        found = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool job_queue_cancel(uint32_t id)
{
    if (s_lock == NULL || id == 0U) return false;

    bool cancelled = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        /* Only a QUEUED job can be cancelled here. A RUNNING job is stopped by
         * the controller, which owns the valves. */
        if (s_slots[i].id == id && s_slots[i].state == JOB_QUEUED) {
            s_slots[i].state = JOB_CANCELLED;
            strncpy(s_slots[i].error, "cancelled", JOB_ERROR_MAX - 1U);
            s_slots[i].error[JOB_ERROR_MAX - 1U] = '\0';
            cancelled = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return cancelled;
}

bool job_queue_unstart(uint32_t id, bool hold)
{
    if (s_lock == NULL || id == 0U) return false;

    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != id) continue;
        if (s_slots[i].state == JOB_RUNNING) {
            s_slots[i].state = JOB_QUEUED;
            s_slots[i].start_ms = 0U;
            s_slots[i].start_g = 0;
            s_slots[i].final_g = 0;
            s_slots[i].held = hold;
            ok = true;
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* Operator queue controls. Held and promoted are plain flags on a WAITING job;
 * neither can be applied to a RUNNING one, because starting order is decided
 * before the controller takes the job and the queue never preempts. */
typedef enum { JOB_FLAG_HELD = 0, JOB_FLAG_PROMOTED } job_flag_t;

static bool job_queue_set_flag(uint32_t id, job_flag_t which, bool value)
{
    if (s_lock == NULL || id == 0U) return false;

    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != id) continue;
        if (s_slots[i].state != JOB_QUEUED) break;  /* RUNNING / terminal: no */
        if (which == JOB_FLAG_HELD) s_slots[i].held = value;
        else s_slots[i].promoted = value;
        ok = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool job_queue_hold(uint32_t id)
{
    return job_queue_set_flag(id, JOB_FLAG_HELD, true);
}

bool job_queue_release(uint32_t id)
{
    return job_queue_set_flag(id, JOB_FLAG_HELD, false);
}

bool job_queue_promote(uint32_t id)
{
    return job_queue_set_flag(id, JOB_FLAG_PROMOTED, true);
}

uint32_t job_queue_cancel_all(void)
{
    if (s_lock == NULL) return 0U;

    uint32_t count = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U && s_slots[i].state == JOB_QUEUED) {
            s_slots[i].state = JOB_CANCELLED;
            strncpy(s_slots[i].error, "cancelled", JOB_ERROR_MAX - 1U);
            s_slots[i].error[JOB_ERROR_MAX - 1U] = '\0';
            count++;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

/* job_queue_cancel_channel_queued() was removed deliberately.
 *
 * It cancelled EVERY queued job on a channel and was reachable from the
 * single-job CANCEL path via dual_dispense_controller_cancel(), so cancelling
 * one target weight cleared its siblings — including duplicate target_g rows.
 * Bulk channel cancellation is not a feature; the only bulk primitive left is
 * job_queue_cancel_all() for the emergency-stop path. Single-job removal is
 * job_queue_cancel(id) / dual_dispense_controller_cancel_job(id). */

bool job_queue_get_running(dispense_job_t *out)
{
    if (s_lock == NULL || out == NULL) return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U && s_slots[i].state == JOB_RUNNING) {
            *out = s_slots[i];   /* copied under the lock, owned by the caller */
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool job_queue_get(uint32_t id, dispense_job_t *out)
{
    if (s_lock == NULL || out == NULL || id == 0U) return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id == id) {
            *out = s_slots[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool job_queue_find_by_remote(uint32_t remote_command_id, job_view_t *out)
{
    if (s_lock == NULL || out == NULL || remote_command_id == 0U) return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U && s_slots[i].remote_command_id == remote_command_id) {
            out->id = s_slots[i].id;
            out->channel_id = s_slots[i].channel_id;
            out->state = s_slots[i].state;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint32_t job_queue_snapshot(dispense_job_t *out, uint32_t max)
{
    if (s_lock == NULL || out == NULL || max == 0U) return 0U;

    dispense_job_t tmp[JOB_QUEUE_MAX];
    uint32_t count = 0U;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0U; i < JOB_QUEUE_MAX; i++) {
        if (s_slots[i].id != 0U) tmp[count++] = s_slots[i];
    }
    xSemaphoreGive(s_lock);

    /* Insertion sort by seq: the FIFO order the caller asked for. N <= 8, so
     * this is cheaper than any general sort and has no allocation. */
    for (uint32_t i = 1U; i < count; i++) {
        dispense_job_t key = tmp[i];
        uint32_t j = i;
        while (j > 0U && tmp[j - 1U].seq > key.seq) {
            tmp[j] = tmp[j - 1U];
            j--;
        }
        tmp[j] = key;
    }

    uint32_t n = (count < max) ? count : max;
    memcpy(out, tmp, n * sizeof(dispense_job_t));
    return n;
}

void job_queue_reset(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1U;
    s_next_seq = 1U;
    xSemaphoreGive(s_lock);
}
