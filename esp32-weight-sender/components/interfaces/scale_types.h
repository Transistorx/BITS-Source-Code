#ifndef SCALE_TYPES_H
#define SCALE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;

    bool has_gross;
    bool has_net;
    bool has_tare;
    bool has_display;
    bool has_unit;
    bool has_status;
    /* True only when the parser decoded the value in the displayed unit. */
    bool gross_is_physical;
    bool display_is_physical;

    double gross;
    double net;
    double tare;
    double display;

    char unit[8];

    // Normalized status
    bool stable;
    bool overload;
    bool underload;
    bool error;
    bool zero;
    bool center_zero;
    bool display_net;

    // Raw protocol status when available
    uint32_t raw_status;
    uint32_t raw_error;

    uint8_t channel_id;
    uint32_t timestamp_ms;
} scale_reading_t;

typedef enum {
    CHANNEL_STATE_DISABLED,
    CHANNEL_STATE_OFFLINE,
    CHANNEL_STATE_ONLINE,
    CHANNEL_STATE_ERROR
} channel_state_t;

typedef struct {
    uint8_t channel_id;
    channel_state_t state;
    scale_reading_t latest_reading;
    uint32_t last_update_ms;
    uint32_t weight_sequence; /* accepted weight-field events, not reads */
    /* Timestamp of the latest actual weight field, not status-only frames. */
    uint32_t gross_update_ms;
    uint32_t display_update_ms;
    uint32_t status_update_ms;
} scale_channel_status_t;

#ifdef __cplusplus
}
#endif

#endif // SCALE_TYPES_H
