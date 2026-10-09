/* Auto-tune two-stage authorization. See include/autotune_auth.h. Pure logic. */

#include "autotune_auth.h"

#include <string.h>

bool autotune_auth_init(autotune_auth_t *a, const autotune_auth_cfg_t *cfg)
{
    if (!a) return false;
    memset(a, 0, sizeof(*a));
    if (!cfg || cfg->max_cmd_ttl_ms == 0 || cfg->arm_window_ms == 0) return false;
    a->cfg = *cfg;
    a->ok = true;
    return true;
}

static autotune_reason_t refuse(autotune_auth_t *a, const autotune_cmd_t *c,
                                autotune_reason_t r)
{
    a->st.refusal = r;
    if (c) a->st.last_cmd_id = c->cmd_id;
    return r;
}

/* Envelope: id, retained, ttl, expiry, auth, duplicate/floor. Consumes the id
 * only when everything passes. */
static autotune_reason_t envelope(autotune_auth_t *a, const autotune_cmd_t *c)
{
    if (!a->ok) return AT_R_BAD_CONFIG;
    if (c->cmd_id == 0) return AT_R_ID_INVALID;
    if (c->retained) return AT_R_RETAINED;
    if (c->ttl_ms == 0 || c->ttl_ms > a->cfg.max_cmd_ttl_ms) return AT_R_TTL_INVALID;
    if (c->age_ms >= c->ttl_ms) return AT_R_CMD_EXPIRED;
    if (!c->auth_ok) return AT_R_AUTH_NOT_OK;
    if (c->cmd_id <= a->floor_id) return AT_R_ID_BELOW_FLOOR;
    for (uint8_t i = 0; i < a->seen_n; i++) {
        if (a->seen[i] == c->cmd_id) return AT_R_ID_DUPLICATE;
    }
    if (a->seen_n < AT_AUTH_SEEN_RING) {
        a->seen[a->seen_n++] = c->cmd_id;
    } else {
        uint32_t ev = a->seen[a->seen_head];
        if (ev > a->floor_id) a->floor_id = ev;
        a->seen[a->seen_head] = c->cmd_id;
        a->seen_head = (uint8_t)((a->seen_head + 1) % AT_AUTH_SEEN_RING);
    }
    return AT_R_NONE;
}

static autotune_reason_t ready_gate(const autotune_ready_t *r)
{
    if (!r->no_active_job) return AT_R_JOB_ACTIVE;
    if (!r->ownership_ok) return AT_R_OWNERSHIP;
    if (!r->no_safety_fault) return AT_R_SAFETY_FAULT;
    if (!r->no_estop) return AT_R_ESTOP;
    if (!r->weight_fresh_valid) return AT_R_WEIGHT_NOT_FRESH;
    if (!r->relays_all_off) return AT_R_RELAYS_NOT_OFF;
    if (!r->no_manual_pump) return AT_R_MANUAL_PUMP;
    return AT_R_NONE;
}

static uint32_t make_token(const autotune_auth_t *a, uint32_t id, uint8_t ch,
                           uint32_t mat, uint32_t epoch)
{
    uint32_t h = id * 2654435761u;
    h ^= epoch * 40503u;
    h ^= ((uint32_t)ch << 24) ^ mat;
    h = h * 2246822519u + a->arm_counter * 3266489917u + 1u;
    return h ? h : 1u;
}

autotune_reason_t autotune_auth_arm(autotune_auth_t *a, const autotune_arm_req_t *r)
{
    if (!a || !r) return AT_R_BAD_CONFIG;
    autotune_reason_t g = envelope(a, &r->cmd);
    if (g) return refuse(a, &r->cmd, g);
    if (a->st.state == AT_AUTH_ARMED || a->st.state == AT_AUTH_RUNNING)
        return refuse(a, &r->cmd, AT_R_BAD_STATE);
    if (r->channel != 1 && r->channel != 2) return refuse(a, &r->cmd, AT_R_CHANNEL_INVALID);
    if (!r->material_selected) return refuse(a, &r->cmd, AT_R_NO_MATERIAL_SELECTED);
    if (r->material_id != (uint32_t)r->channel) return refuse(a, &r->cmd, AT_R_MATERIAL_MISMATCH);
    if (!r->scale_under_nozzle) return refuse(a, &r->cmd, AT_R_NO_SCALE_UNDER_NOZZLE);
    if (!r->container_present) return refuse(a, &r->cmd, AT_R_NO_CONTAINER);
    g = ready_gate(&r->ready);
    if (g) return refuse(a, &r->cmd, g);

    a->arm_counter++;
    a->st.state = AT_AUTH_ARMED;
    a->st.reason = AT_R_NONE;
    a->st.refusal = AT_R_NONE;
    a->st.channel = r->channel;
    a->st.material_id = r->material_id;
    a->st.arm_cmd_id = r->cmd.cmd_id;
    a->st.last_cmd_id = r->cmd.cmd_id;
    a->st.arm_rx_ms = r->cmd.rx_ms;
    a->st.boot_epoch = r->cmd.boot_epoch;
    a->st.token = make_token(a, r->cmd.cmd_id, r->channel, r->material_id, r->cmd.boot_epoch);
    return AT_R_NONE;
}

static void do_cancel(autotune_auth_t *a, autotune_reason_t why)
{
    if (a->st.state == AT_AUTH_ARMED || a->st.state == AT_AUTH_RUNNING) {
        a->st.state = AT_AUTH_CANCELLED;
        a->st.reason = why;
        a->st.token = 0;
    }
}

autotune_reason_t autotune_auth_start(autotune_auth_t *a, const autotune_start_req_t *r)
{
    if (!a || !r) return AT_R_BAD_CONFIG;
    autotune_reason_t g = envelope(a, &r->cmd);
    if (g) return refuse(a, &r->cmd, g);
    if (!a->cfg.hw_enable) return refuse(a, &r->cmd, AT_R_HW_NOT_APPROVED);
    if (a->st.state != AT_AUTH_ARMED) return refuse(a, &r->cmd, AT_R_NO_ARM);
    if ((uint32_t)(r->cmd.rx_ms - a->st.arm_rx_ms) >= a->cfg.arm_window_ms) {
        do_cancel(a, AT_R_ARM_EXPIRED);
        return refuse(a, &r->cmd, AT_R_ARM_EXPIRED);
    }
    if (r->token == 0 || r->token != a->st.token) return refuse(a, &r->cmd, AT_R_TOKEN_MISMATCH);
    if (r->channel != a->st.channel) return refuse(a, &r->cmd, AT_R_WRONG_CHANNEL);
    if (r->material_id != a->st.material_id) return refuse(a, &r->cmd, AT_R_WRONG_MATERIAL);
    if (r->cmd.boot_epoch != a->st.boot_epoch) {
        do_cancel(a, AT_R_RESTART);
        return refuse(a, &r->cmd, AT_R_RESTART_EPOCH);
    }
    g = ready_gate(&r->ready);
    if (g) return refuse(a, &r->cmd, g);

    a->st.state = AT_AUTH_RUNNING;
    a->st.reason = AT_R_NONE;
    a->st.refusal = AT_R_NONE;
    a->st.last_cmd_id = r->cmd.cmd_id;
    return AT_R_NONE;
}

bool autotune_auth_cancel(autotune_auth_t *a)
{
    if (!a) return true;   /* nothing to cancel; never an error */
    do_cancel(a, AT_R_USER_CANCEL);
    return true;
}

void autotune_auth_tick(autotune_auth_t *a, const autotune_inputs_t *in)
{
    if (!a) return;
    if (a->st.state != AT_AUTH_ARMED && a->st.state != AT_AUTH_RUNNING) return;
    if (!in) { do_cancel(a, AT_R_COMM_LOSS); return; }   /* fail closed */
    autotune_reason_t why = AT_R_NONE;
    if (in->boot_epoch != a->st.boot_epoch) why = AT_R_RESTART;
    else if (in->estop) why = AT_R_ESTOP;
    else if (in->safety_fault) why = AT_R_SAFETY_FAULT;
    else if (!in->ownership_ok) why = AT_R_OWNERSHIP_LOST;
    else if (in->manual_pump_active) why = AT_R_MANUAL_PUMP;
    else if (in->job_active) why = AT_R_JOB_ACTIVE;
    else if (!in->comm_ok) why = AT_R_COMM_LOSS;
    else if (!in->weight_link_ok) why = AT_R_WEIGHT_LINK_LOST;
    else if (!in->weight_fresh_valid) why = AT_R_WEIGHT_STALE;
    else if (a->st.state == AT_AUTH_ARMED &&
             (uint32_t)(in->now_ms - a->st.arm_rx_ms) >= a->cfg.arm_window_ms)
        why = AT_R_ARM_EXPIRED;
    if (why) do_cancel(a, why);
}

void autotune_auth_finish(autotune_auth_t *a)
{
    if (a && a->st.state == AT_AUTH_RUNNING) {
        a->st.state = AT_AUTH_IDLE;
        a->st.token = 0;
    }
}
