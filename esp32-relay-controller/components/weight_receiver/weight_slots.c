#include "weight_slots.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t s_slot;

bool weight_slots_init(void)
{
    if (s_slot == NULL) s_slot = xQueueCreate(1, sizeof(weight_slot_sample_t));
    if (s_slot == NULL) return false;
    (void)xQueueReset(s_slot);
    return true;
}

void weight_slots_post(const weight_msg_t *msg, uint32_t acquired_ms)
{
    if (msg == NULL || s_slot == NULL) return;
    weight_slot_sample_t s = { .msg = *msg, .acquired_ms = acquired_ms };
    (void)xQueueOverwrite(s_slot, &s);
}

bool weight_slots_take(weight_slot_sample_t *out)
{
    if (out == NULL || s_slot == NULL) return false;
    return xQueueReceive(s_slot, out, 0) == pdTRUE;
}
