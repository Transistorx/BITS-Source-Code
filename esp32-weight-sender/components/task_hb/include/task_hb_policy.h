#ifndef TASK_HB_POLICY_H
#define TASK_HB_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TASK_HB_ACT_NONE = 0,
    TASK_HB_ACT_LINK_RESTART,
    TASK_HB_ACT_REBOOT,
    TASK_HB_ACT_LINK_RESTART_DEGRADED
} task_hb_action_t;

typedef struct {
    uint32_t link_restart_ms;
    uint32_t restart_window_ms;
    uint32_t stable_ms;
    uint8_t reboot_cap;
} task_hb_policy_cfg_t;

typedef struct {
    bool armed;
    uint32_t armed_ms;
    uint8_t reboots;
    bool degraded;
} task_hb_policy_state_t;

bool task_hb_policy_link_live(task_hb_policy_state_t *s, const task_hb_policy_cfg_t *cfg,
                              uint32_t now_ms, bool link_idle);

task_hb_action_t task_hb_policy_step(task_hb_policy_state_t *s, const task_hb_policy_cfg_t *cfg,
                                     uint32_t now_ms, bool link_idle, bool pub_frozen_long);

#ifdef __cplusplus
}
#endif

#endif
