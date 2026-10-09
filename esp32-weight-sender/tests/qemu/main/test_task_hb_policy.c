#include "test_task_hb_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "task_hb_policy.h"
#include "test_harness.h"

static const task_hb_policy_cfg_t k_cfg = {
    .link_restart_ms = 30000U,
    .restart_window_ms = 600000U,
    .stable_ms = 3600000U,
    .reboot_cap = 3U,
};

static task_hb_action_t step(task_hb_policy_state_t *s, uint32_t now, bool idle, bool frozen)
{
    return task_hb_policy_step(s, &k_cfg, now, idle, frozen);
}

static void test_escalation_chain(void)
{
    task_hb_policy_state_t s;
    memset(&s, 0, sizeof(s));

    test_check(step(&s, 1000U, true, true) == TASK_HB_ACT_NONE && !s.armed,
               "REQ-WMQ-52_idle_link_frozen_pub_ignored");
    test_check(step(&s, 2000U, false, false) == TASK_HB_ACT_NONE && !s.armed,
               "REQ-WMQ-52_live_link_not_frozen_no_action");
    test_check(step(&s, 40000U, false, true) == TASK_HB_ACT_LINK_RESTART && s.armed &&
                   s.armed_ms == 40000U && s.reboots == 0U,
               "REQ-WMQ-52_first_freeze_restarts_link");
    test_check(step(&s, 40000U + 29999U, false, true) == TASK_HB_ACT_NONE,
               "REQ-WMQ-52_frozen_inside_restart_cooldown_waits");
    test_check(step(&s, 40000U + 30000U, false, true) == TASK_HB_ACT_REBOOT && s.reboots == 1U,
               "REQ-WMQ-52_second_freeze_inside_window_reboots_and_counts");
}

static void test_window_expiry_clears_arm(void)
{
    task_hb_policy_state_t s;
    memset(&s, 0, sizeof(s));

    test_check(step(&s, 100000U, false, true) == TASK_HB_ACT_LINK_RESTART, "REQ-WMQ-52_arm_setup");
    test_check(task_hb_policy_link_live(&s, &k_cfg, 100000U + 599999U, true),
               "REQ-WMQ-52_idle_link_counts_as_live_inside_window");
    test_check(!task_hb_policy_link_live(&s, &k_cfg, 100000U + 600000U, true) && !s.armed,
               "REQ-WMQ-52_window_expiry_clears_arm_so_idle_link_not_supervised");
    test_check(step(&s, 100000U + 600000U, false, true) == TASK_HB_ACT_LINK_RESTART && s.reboots == 0U,
               "REQ-WMQ-52_freeze_after_window_restarts_link_not_reboot");
}

static void test_reboot_cap_degrades(void)
{
    task_hb_policy_state_t s;
    memset(&s, 0, sizeof(s));
    s.reboots = 3U;

    test_check(step(&s, 50000U, false, true) == TASK_HB_ACT_LINK_RESTART && !s.degraded,
               "REQ-WMQ-52_capped_first_freeze_still_restarts_link");
    test_check(step(&s, 50000U + 30000U, false, true) == TASK_HB_ACT_LINK_RESTART_DEGRADED &&
                   s.degraded && s.reboots == 3U && s.armed_ms == 80000U,
               "REQ-WMQ-52_cap_reached_degrades_instead_of_reboot");
    test_check(step(&s, 80000U + 10000U, false, true) == TASK_HB_ACT_NONE,
               "REQ-WMQ-52_degraded_link_restart_rate_bounded");
    test_check(step(&s, 80000U + 30000U, false, true) == TASK_HB_ACT_LINK_RESTART_DEGRADED &&
                   s.reboots == 3U,
               "REQ-WMQ-52_degraded_never_reboots_again");
    test_check(step(&s, 3600000U, false, false) == TASK_HB_ACT_NONE && s.reboots == 0U && !s.degraded,
               "REQ-WMQ-52_stable_uptime_clears_counter_and_degraded");
}

static void test_cap_zero_never_reboots(void)
{
    task_hb_policy_cfg_t cfg = k_cfg;
    task_hb_policy_state_t s;
    cfg.reboot_cap = 0U;
    memset(&s, 0, sizeof(s));

    test_check(task_hb_policy_step(&s, &cfg, 1000U, false, true) == TASK_HB_ACT_LINK_RESTART,
               "REQ-WMQ-52_cap_zero_first_restart");
    test_check(task_hb_policy_step(&s, &cfg, 31000U, false, true) == TASK_HB_ACT_LINK_RESTART_DEGRADED &&
                   s.reboots == 0U,
               "REQ-WMQ-52_cap_zero_never_reboots");
}

static void test_wraparound_and_nulls(void)
{
    task_hb_policy_state_t s;
    memset(&s, 0, sizeof(s));

    test_check(step(&s, 0xFFFFFF00U, false, true) == TASK_HB_ACT_LINK_RESTART, "REQ-WMQ-52_wrap_arm");
    test_check(step(&s, 0x00000100U, false, true) == TASK_HB_ACT_NONE, "REQ-WMQ-52_wrap_cooldown_holds");
    test_check(step(&s, 0x00007600U, false, true) == TASK_HB_ACT_REBOOT, "REQ-WMQ-52_wrap_escalates");
    test_check(task_hb_policy_step(NULL, &k_cfg, 0U, false, true) == TASK_HB_ACT_NONE &&
                   task_hb_policy_step(&s, NULL, 0U, false, true) == TASK_HB_ACT_NONE &&
                   !task_hb_policy_link_live(NULL, &k_cfg, 0U, false) &&
                   !task_hb_policy_link_live(&s, NULL, 0U, false),
               "REQ-WMQ-52_null_args_refused");
}

void test_task_hb_policy_run(void)
{
    test_escalation_chain();
    test_window_expiry_clears_arm();
    test_reboot_cap_degrades();
    test_cap_zero_never_reboots();
    test_wraparound_and_nulls();
}
