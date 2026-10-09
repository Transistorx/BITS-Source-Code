#include "cas_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "esp_log.h"

static bool copy_unit_token(const char *text, scale_reading_t *reading)
{
    while (*text != '\0' && isspace((unsigned char)*text)) text++;
    size_t i = 0;
    while (i < sizeof(reading->unit) - 1U &&
           isalpha((unsigned char)text[i])) {
        reading->unit[i] = text[i];
        i++;
    }
    if (i == 0U || isalpha((unsigned char)text[i])) return false;
    size_t unit_len = i;
    while (text[i] != '\0' && isspace((unsigned char)text[i])) i++;
    if (text[i] != '\0') return false;
    reading->unit[unit_len] = '\0';
    reading->has_unit = true;
    return true;
}

static bool parse_weight_value(const char *payload, bool literal,
                               double *out_value, scale_reading_t *reading)
{
    if (literal) {
        char *end = NULL;
        float value = strtof(payload, &end);
        if (end == payload || !isfinite(value)) return false;

        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        char *unit_start = end;
        while (isalpha((unsigned char)*end)) end++;
        size_t unit_len = (size_t)(end - unit_start);
        if (unit_len == 0U || unit_len >= sizeof(reading->unit)) return false;
        while (*end != '\0' && isspace((unsigned char)*end)) end++;
        if (*end != '\0' && strcmp(end, "G") != 0 &&
            strcmp(end, "N") != 0 && strcmp(end, "T") != 0) {
            return false;
        }

        *out_value = value;
        memcpy(reading->unit, unit_start, unit_len);
        reading->unit[unit_len] = '\0';
        reading->has_unit = true;
        return true;
    }

    while (*payload != '\0' && isspace((unsigned char)*payload)) payload++;
    const char *cursor = payload;
    size_t digits = 0U;
    while (isxdigit((unsigned char)cursor[digits])) digits++;
    if (digits == 0U || digits > 8U) return false;
    cursor += digits;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) cursor++;
    if (*cursor != '\0') return false;

    unsigned long raw = strtoul(payload, NULL, 16);
    *out_value = (double)(int32_t)(uint32_t)raw;
    return true;
}

static bool parse_hex_word(const char *text, uint32_t *out_value)
{
    size_t digits = 0U;
    while (isxdigit((unsigned char)text[digits])) digits++;
    if (digits == 0U || digits > 8U) return false;
    const char *tail = text + digits;
    while (*tail != '\0' && isspace((unsigned char)*tail)) tail++;
    if (*tail != '\0') return false;
    *out_value = (uint32_t)strtoul(text, NULL, 16);
    return true;
}

static bool is_hex_digit_string(const char *text, size_t length)
{
    for (size_t i = 0U; i < length; i++) {
        if (!isxdigit((unsigned char)text[i])) return false;
    }
    return true;
}

static bool cas_parse_frame(const char* buffer, scale_reading_t* out_reading) {
    if (!buffer || !out_reading) return false;
    
    memset(out_reading, 0, sizeof(scale_reading_t));
    
    // Response looks like: "81050026: 10.00 kg G" or "81110021:00000000".
    if (strlen(buffer) < 10U || strncmp(buffer, "81", 2) != 0 ||
        buffer[8] != ':' || !is_hex_digit_string(buffer + 2, 6U)) {
        return false;
    }

    const char* cmd = buffer + 2;
    if (strncmp(cmd, "05", 2) != 0 && strncmp(cmd, "11", 2) != 0) return false;
    
    const char* reg = buffer + 4;
    const char* payload = buffer + 9;
    while (*payload == ' ') payload++;
    
    bool is_literal = (strncmp(cmd, "05", 2) == 0);
    bool recognized_register = true;
    
    if (strncmp(reg, "0026", 4) == 0) { // Gross
        if (!parse_weight_value(payload, is_literal, &out_reading->gross, out_reading)) return false;
        out_reading->has_gross = true;
        out_reading->gross_is_physical = is_literal;
    } else if (strncmp(reg, "0027", 4) == 0) { // Net
        if (!parse_weight_value(payload, is_literal, &out_reading->net, out_reading)) return false;
        out_reading->has_net = true;
    } else if (strncmp(reg, "0028", 4) == 0) { // Tare
        if (!parse_weight_value(payload, is_literal, &out_reading->tare, out_reading)) return false;
        out_reading->has_tare = true;
    } else if (strncmp(reg, "0024", 4) == 0) { // Display
        if (!parse_weight_value(payload, is_literal, &out_reading->display, out_reading)) return false;
        out_reading->has_display = true;
        out_reading->display_is_physical = is_literal;
    } else if (strncmp(reg, "0129", 4) == 0) { // Unit
        if (is_literal) {
            if (!copy_unit_token(payload, out_reading)) return false;
        } else {
            recognized_register = false;
        }
    } else if (strncmp(reg, "0021", 4) == 0) { // System Status
        uint32_t status = 0U;
        if (!parse_hex_word(payload, &status)) return false;
        out_reading->raw_status = status;
        
        out_reading->overload = (status & (1UL << 17)) != 0;
        out_reading->underload = (status & (1UL << 16)) != 0;
        out_reading->error = (status & (1UL << 15)) != 0;
        out_reading->stable = (status & (1UL << 12)) == 0; // Motion is bit 12
        out_reading->center_zero = (status & (1UL << 11)) != 0;
        out_reading->zero = (status & (1UL << 10)) != 0;
        out_reading->display_net = (status & (1UL << 9)) != 0;
        
        out_reading->has_status = true;
    } else if (strncmp(reg, "0022", 4) == 0) { // System Error
        if (!parse_hex_word(payload, &out_reading->raw_error)) return false;
        // We consider this part of status updates
        out_reading->has_status = true; 
    } else {
        recognized_register = false;
    }

    if (!recognized_register) return false;
    out_reading->valid = true;
    return true;
}

const scale_protocol_t cas_protocol = {
    .name = "CAS_X320",
    .frame_end = "\r\n",
    .parse_frame = cas_parse_frame,
    .build_poll_request = NULL
};
