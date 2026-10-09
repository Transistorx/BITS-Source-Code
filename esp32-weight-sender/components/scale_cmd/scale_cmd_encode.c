#include "scale_cmd.h"

/*
 * !!! VERIFICATION REQUIRED !!!
 * The CAS CI-150A remote ZERO/TARE frame is NOT documented anywhere in this
 * repository (docs/cas-audit and the RS-232 golden baseline only describe the
 * scale's outbound weight stream; ZERO/TARE are physical keys). No bytes are
 * emitted. Do not fill this in from memory or from another CAS model.
 */
scale_enc_status_t scale_command_encode(scale_cmd_type_t type, uint8_t channel,
                                        uint8_t *frame, size_t cap, size_t *len)
{
    (void)type;
    (void)channel;
    (void)frame;
    (void)cap;
    if (len != NULL) *len = 0U;
    return SCALE_ENC_NOT_VERIFIED;
}
