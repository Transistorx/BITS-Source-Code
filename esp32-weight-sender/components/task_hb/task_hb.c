#include "task_hb.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

static atomic_uint s_count[TASK_HB_COUNT];
static uint32_t s_seen[TASK_HB_COUNT];
static uint32_t s_changed_ms[TASK_HB_COUNT];

static const char *const s_names[TASK_HB_COUNT] = { "WEIGHT_TX", "MQTT_PUB", "SCALE_CMD" };

void task_hb_bump(task_hb_id_t id)
{
    if ((unsigned)id >= (unsigned)TASK_HB_COUNT) return;
    atomic_fetch_add(&s_count[id], 1U);
}

uint32_t task_hb_count(task_hb_id_t id)
{
    if ((unsigned)id >= (unsigned)TASK_HB_COUNT) return 0U;
    return atomic_load(&s_count[id]);
}

const char *task_hb_name(task_hb_id_t id)
{
    if ((unsigned)id >= (unsigned)TASK_HB_COUNT) return "UNKNOWN";
    return s_names[id];
}

uint32_t task_hb_check(uint32_t now_ms, uint32_t frozen_ms, uint32_t *frozen_mask)
{
    uint32_t mask = 0U;
    uint32_t frozen = 0U;

    for (unsigned i = 0U; i < (unsigned)TASK_HB_COUNT; i++) {
        uint32_t c = atomic_load(&s_count[i]);
        if (c != s_seen[i]) {
            s_seen[i] = c;
            s_changed_ms[i] = now_ms;
            continue;
        }
        if ((uint32_t)(now_ms - s_changed_ms[i]) >= frozen_ms) {
            mask |= 1U << i;
            frozen++;
        }
    }
    if (frozen_mask != NULL) *frozen_mask = mask;
    return frozen;
}
