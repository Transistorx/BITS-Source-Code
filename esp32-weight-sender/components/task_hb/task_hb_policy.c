#include "task_hb_policy.h"

#include <stddef.h>

static void expire_arm(task_hb_policy_state_t *s, const task_hb_policy_cfg_t *cfg, uint32_t now_ms)
{
    if (s->armed && (uint32_t)(now_ms - s->armed_ms) >= cfg->restart_window_ms) s->armed = false;
}

bool task_hb_policy_link_live(task_hb_policy_state_t *s, const task_hb_policy_cfg_t *cfg,
                              uint32_t now_ms, bool link_idle)
{
    if (s == NULL || cfg == NULL) return false;
    expire_arm(s, cfg, now_ms);
    return !link_idle || s->armed;
}

task_hb_action_t task_hb_policy_step(task_hb_policy_state_t *s, const task_hb_policy_cfg_t *cfg,
                                     uint32_t now_ms, bool link_idle, bool pub_frozen_long)
{
    if (s == NULL || cfg == NULL) return TASK_HB_ACT_NONE;
    if (s->reboots != 0U && now_ms >= cfg->stable_ms) {
        s->reboots = 0U;
        s->degraded = false;
    }
    if (!task_hb_policy_link_live(s, cfg, now_ms, link_idle) || !pub_frozen_long) return TASK_HB_ACT_NONE;
    if (s->armed && (uint32_t)(now_ms - s->armed_ms) < cfg->link_restart_ms) return TASK_HB_ACT_NONE;
    if (!s->armed) {
        s->armed = true;
        s->armed_ms = now_ms;
        return TASK_HB_ACT_LINK_RESTART;
    }
    if (s->reboots >= cfg->reboot_cap) {
        s->degraded = true;
        s->armed_ms = now_ms;
        return TASK_HB_ACT_LINK_RESTART_DEGRADED;
    }
    s->reboots++;
    return TASK_HB_ACT_REBOOT;
}
