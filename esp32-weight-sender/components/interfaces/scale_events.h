#ifndef SCALE_EVENTS_H
#define SCALE_EVENTS_H

#include "scale_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCALE_EVENT_READING,
    SCALE_EVENT_ONLINE,
    SCALE_EVENT_OFFLINE,
    SCALE_EVENT_PROTOCOL_ERROR,
    SCALE_EVENT_TRANSPORT_ERROR
} scale_event_type_t;

typedef struct {
    uint8_t channel_id;
    scale_event_type_t type;
    scale_reading_t reading; // Valid only if type == SCALE_EVENT_READING
} scale_event_t;

#ifdef __cplusplus
}
#endif

#endif // SCALE_EVENTS_H

