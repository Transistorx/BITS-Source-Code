#ifndef WEIGHT_SLOTS_H
#define WEIGHT_SLOTS_H

#include "weight_receiver.h"

#include <stdbool.h>
#include <stdint.h>

/* Hand-off from the weight sink (any task) to the supervisor. Single-scale
 * operation (CONTRACT 9.11): there is ONE scale, so ONE overwrite slot that keeps
 * only the newest sample. No per-channel slot and no pump attribution exist. */
typedef struct { weight_msg_t msg; uint32_t acquired_ms; } weight_slot_sample_t;

bool weight_slots_init(void);
/* Never blocks. Replaces the older sample. */
void weight_slots_post(const weight_msg_t *msg, uint32_t acquired_ms);
/* Pending sample (consumed), or false when empty. */
bool weight_slots_take(weight_slot_sample_t *out);

#endif
