#ifndef TELEMETRY_JSON_H
#define TELEMETRY_JSON_H

/* Component-private: the tiny structural JSON walkers of telemetry_client.c, shared
 * with run_log.c so run/ack is parsed exactly like a command (a key spelled inside a
 * string or a nested object never matches). */

/* '}' that closes the brace-balanced object starting at *p, or NULL if unterminated. */
const char *tc_json_object_end(const char *p);
/* Members named `key` directly inside the object [obj..end]: count, and the first
 * one's value start in *val (NULL when absent). */
int tc_json_object_member(const char *obj, const char *end, const char *key, const char **val);

#endif
