#ifndef TASK_HB_H
#define TASK_HB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TASK_HB_WEIGHT_TX = 0,
    TASK_HB_MQTT_PUB,
    TASK_HB_SCALE_CMD,
    TASK_HB_COUNT
} task_hb_id_t;

void task_hb_bump(task_hb_id_t id);

uint32_t task_hb_count(task_hb_id_t id);

const char *task_hb_name(task_hb_id_t id);

uint32_t task_hb_check(uint32_t now_ms, uint32_t frozen_ms, uint32_t *frozen_mask);

#ifdef __cplusplus
}
#endif

#endif
