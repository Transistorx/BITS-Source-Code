#ifndef ASUKI_PARSER_H
#define ASUKI_PARSER_H

#include "protocol_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Asuki protocol adapter instance.
 * Can be registered with a Scale Reader.
 */
extern const scale_protocol_t asuki_protocol;

#ifdef __cplusplus
}
#endif

#endif // ASUKI_PARSER_H
