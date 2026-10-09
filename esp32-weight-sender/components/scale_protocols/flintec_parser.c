#include "flintec_parser.h"
#include <string.h>

static bool flintec_parse_frame(const char *buffer, scale_reading_t *out_reading)
{
    // TODO: Implement Flintec FT210 specific parsing logic
    return false;
}

const scale_protocol_t flintec_protocol = {
    .name = "Flintec FT210",
    .frame_end = "\n",
    .parse_frame = flintec_parse_frame,
    .build_poll_request = NULL
};

