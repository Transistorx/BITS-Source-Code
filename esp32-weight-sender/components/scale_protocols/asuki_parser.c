#include "asuki_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static const char* skip_whitespace(const char *str) {
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

static bool parse_float_after_label(const char *line, const char *label, float *out,
                                    scale_reading_t *reading) {
    size_t label_len = strlen(label);
    if (strncmp(line, label, label_len) != 0) {
        return false;
    }
    if (line[label_len] != '\0' && !isspace((unsigned char)line[label_len]) &&
        line[label_len] != ':') {
        return false;
    }
    
    const char *payload = line + label_len;
    payload = skip_whitespace(payload);
    if (*payload == ':') {
        payload = skip_whitespace(payload + 1);
    }
    
    if (*payload == '\0') {
        return false;
    }
    
    char *endptr = NULL;
    float value = strtof(payload, &endptr);
    if (endptr == payload || !isfinite(value)) {
        return false;
    }

    /* A scale field may end after its number, or have one alphabetic unit. */
    const char *unit = skip_whitespace(endptr);
    size_t unit_len = 0;
    while (unit_len < sizeof(reading->unit) - 1U &&
           isalpha((unsigned char)unit[unit_len])) {
        unit_len++;
    }
    const char *tail = skip_whitespace(unit + unit_len);
    if (*tail != '\0' ||
        (unit_len > 0U && isalpha((unsigned char)unit[unit_len]))) {
        return false;
    }
    
    *out = value;

    /* Preserve the unit from frames such as "GS 10.000kg". */
    if (unit_len > 0) {
        memcpy(reading->unit, unit, unit_len);
        reading->unit[unit_len] = '\0';
        reading->has_unit = true;
    }
    return true;
}

static bool asuki_parse_frame(const char* buffer, scale_reading_t* out_reading) {
    if (!buffer || !out_reading) return false;

    // Reset reading
    memset(out_reading, 0, sizeof(scale_reading_t));
    
    char frame_copy[256];
    strncpy(frame_copy, buffer, sizeof(frame_copy) - 1);
    frame_copy[sizeof(frame_copy) - 1] = '\0';
    
    char *saveptr = NULL;
    char *line = strtok_r(frame_copy, "\n", &saveptr);
    
    bool found_any = false;

    while (line != NULL) {
        line = (char*)skip_whitespace(line);
        float fval;

        if (parse_float_after_label(line, "GS", &fval, out_reading)) {
            out_reading->gross = fval;
            out_reading->has_gross = true;
            out_reading->gross_is_physical = true;
            found_any = true;
        } else if (parse_float_after_label(line, "NT", &fval, out_reading)) {
            out_reading->net = fval;
            out_reading->has_net = true;
            found_any = true;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }
    
    out_reading->valid = found_any;
    return found_any;
}

const scale_protocol_t asuki_protocol = {
    .name = "ASUKI_QW",
    .frame_end = "\n",
    .parse_frame = asuki_parse_frame,
    .build_poll_request = NULL
};
