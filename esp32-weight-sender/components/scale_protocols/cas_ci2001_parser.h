#ifndef CAS_CI2001_PARSER_H
#define CAS_CI2001_PARSER_H

#include "protocol_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const scale_protocol_t cas_ci2001_protocol;

/* Golden parsing check including opaque, undocumented record octets. */
bool cas_ci2001_parser_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* CAS_CI2001_PARSER_H */
