#ifndef PROTOCOL_INTERFACE_H
#define PROTOCOL_INTERFACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "scale_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a complete frame was rejected. Diagnostics only: never affects what is
 * accepted. Protocols without a classifier report SCALE_REJ_OTHER. */
typedef enum {
    SCALE_REJ_NONE = 0,
    SCALE_REJ_LENGTH,   /* body length is not the protocol's fixed length */
    SCALE_REJ_HEADER,   /* separators, GS/NT field or device ID */
    SCALE_REJ_STATUS,   /* status word is not US / ST / OL */
    SCALE_REJ_FIELD,    /* weight or unit field (the format has no checksum) */
    SCALE_REJ_OTHER
} scale_reject_reason_t;

typedef struct {
    const char* name;
    
    /* 
     * Optional sequence that defines the end of a frame (e.g. "\n" or "\r\n").
     * If NULL, defaults to "\n".
     */
    const char* frame_end;
    
    /* 
     * Parse a complete frame buffer into a normalized reading.
     * Returns true if parsing was successful and reading is populated.
     */
    bool (*parse_frame)(const char* buffer, scale_reading_t* out_reading);

    /*
     * Optional length-aware parser for protocols that contain binary fields
     * in an otherwise line-delimited frame. The reader removes the configured
     * line terminator before invoking this callback.
     */
    bool (*parse_frame_with_length)(const uint8_t* buffer, size_t length,
                                    scale_reading_t* out_reading);

    /*
     * Optional: Generate a poll request to send to the scale.
     * Returns true if a request was written to out_buf.
     */
    bool (*build_poll_request)(uint8_t channel_id, char* out_buf, size_t max_len, size_t* out_len);

    /*
     * Optional: classify why parse_frame_with_length() rejects a body.
     * Used for per-reason counters only.
     */
    scale_reject_reason_t (*classify_reject)(const uint8_t* buffer, size_t length);
} scale_protocol_t;

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_INTERFACE_H
