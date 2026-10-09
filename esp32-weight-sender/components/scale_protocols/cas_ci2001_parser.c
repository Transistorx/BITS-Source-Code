#include "cas_ci2001_parser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * CAS CI-150A / CI-2001 shared 22-byte ASCII stream frame (CR/LF stripped
 * by scale_reader before this fixed-length body is parsed).
 *
 * Layout (20 bytes):
 *   0-1   status  US | ST | OL
 *   2     ','
 *   3-4   GS | NT
 *   5     ','
 *   6     device ID  (raw byte 0..99; 0x0A collides with LF)
 *   7     lamp status octet
 *   8     ','
 *   9-16  weight  (8 bytes, right-aligned ASCII)
 *   17    pad
 *   18-19 unit    kg | lb  (CI-150A may emit "t" for tonnes)
 */
#define CI2001_FRAME_BODY_SIZE 20U

#define CI2001_STATUS_OFFSET 0U
#define CI2001_GROSS_NET_OFFSET 3U
#define CI2001_DEVICE_ID_OFFSET 6U
#define CI2001_LAMP_STATUS_OFFSET 7U
#define CI2001_WEIGHT_OFFSET 9U
#define CI2001_WEIGHT_SIZE 8U
#define CI2001_UNIT_OFFSET 18U

static bool parse_weight_field(const uint8_t* bytes, double* out_weight)
{
    char text[CI2001_WEIGHT_SIZE + 1U];
    bool has_digit = false;
    bool has_decimal = false;
    size_t start = 0U;
    size_t field_end = CI2001_WEIGHT_SIZE;

    if (bytes == NULL || out_weight == NULL) return false;

    /* The CI-2001 right-aligns the number in its fixed 8-byte field. */
    while (start < field_end && bytes[start] == (uint8_t)' ') start++;
    while (field_end > start && bytes[field_end - 1U] == (uint8_t)' ') field_end--;
    if (start == field_end) return false;

    size_t text_length = field_end - start;
    for (size_t i = 0U; i < text_length; i++) {
        uint8_t byte = bytes[start + i];
        if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') {
            has_digit = true;
        } else if (byte == (uint8_t)'.' && !has_decimal) {
            has_decimal = true;
        } else if (byte == (uint8_t)'-' && i == 0U) {
            /* The manual's weight examples place a negative sign first. */
        } else {
            return false;
        }
        text[i] = (char)byte;
    }

    if (!has_digit) return false;
    text[text_length] = '\0';

    char* parse_end = NULL;
    double value = strtod(text, &parse_end);
    if (parse_end != text + text_length || !isfinite(value)) return false;

    *out_weight = value;
    return true;
}

static bool ascii_is_lower_or_upper(uint8_t byte, char lower)
{
    return byte == (uint8_t)lower ||
           byte == (uint8_t)(lower - 'a' + 'A');
}

/* Single decoder: the accept path and the reject classifier cannot diverge.
 * out_reading may be NULL (classification only). */
static scale_reject_reason_t ci2001_decode(const uint8_t* frame, size_t length,
                                           scale_reading_t* out_reading)
{
    if (frame == NULL) return SCALE_REJ_OTHER;
    if (length != CI2001_FRAME_BODY_SIZE) return SCALE_REJ_LENGTH;

    if (frame[2U] != (uint8_t)',' || frame[5U] != (uint8_t)',' ||
        frame[8U] != (uint8_t)',') {
        return SCALE_REJ_HEADER;
    }

    bool status_unstable = frame[CI2001_STATUS_OFFSET] == (uint8_t)'U' &&
                           frame[CI2001_STATUS_OFFSET + 1U] == (uint8_t)'S';
    bool status_stable = frame[CI2001_STATUS_OFFSET] == (uint8_t)'S' &&
                         frame[CI2001_STATUS_OFFSET + 1U] == (uint8_t)'T';
    bool status_overload = frame[CI2001_STATUS_OFFSET] == (uint8_t)'O' &&
                           frame[CI2001_STATUS_OFFSET + 1U] == (uint8_t)'L';
    if (!status_unstable && !status_stable && !status_overload) return SCALE_REJ_STATUS;

    bool is_gross = frame[CI2001_GROSS_NET_OFFSET] == (uint8_t)'G' &&
                    frame[CI2001_GROSS_NET_OFFSET + 1U] == (uint8_t)'S';
    bool is_net = frame[CI2001_GROSS_NET_OFFSET] == (uint8_t)'N' &&
                  frame[CI2001_GROSS_NET_OFFSET + 1U] == (uint8_t)'T';
    if (!is_gross && !is_net) return SCALE_REJ_HEADER;

    uint8_t device_id = frame[CI2001_DEVICE_ID_OFFSET];
    uint8_t lamp_status = frame[CI2001_LAMP_STATUS_OFFSET];
    /* The supplied manual shows this octet but does not define its bits. */
    if (device_id > 99U) return SCALE_REJ_HEADER;

    bool unit_kg = ascii_is_lower_or_upper(frame[CI2001_UNIT_OFFSET], 'k') &&
                   ascii_is_lower_or_upper(frame[CI2001_UNIT_OFFSET + 1U], 'g');
    bool unit_lb = ascii_is_lower_or_upper(frame[CI2001_UNIT_OFFSET], 'l') &&
                   ascii_is_lower_or_upper(frame[CI2001_UNIT_OFFSET + 1U], 'b');
    if (!unit_kg && !unit_lb) return SCALE_REJ_FIELD;

    double weight = 0.0;
    if (!parse_weight_field(&frame[CI2001_WEIGHT_OFFSET], &weight)) return SCALE_REJ_FIELD;

    if (out_reading == NULL) return SCALE_REJ_NONE;

    memset(out_reading, 0, sizeof(*out_reading));
    out_reading->valid = true;
    out_reading->has_display = true;
    out_reading->display = weight;
    out_reading->display_is_physical = true;
    out_reading->has_unit = true;
    out_reading->unit[0] = unit_kg ? 'k' : 'l';
    out_reading->unit[1] = unit_kg ? 'g' : 'b';
    out_reading->unit[2] = '\0';

    if (is_gross) {
        out_reading->has_gross = true;
        out_reading->gross = weight;
        out_reading->gross_is_physical = true;
    } else {
        out_reading->has_net = true;
        out_reading->net = weight;
    }

    out_reading->has_status = true;
    out_reading->stable = status_stable;
    out_reading->overload = status_overload;
    out_reading->zero = weight == 0.0;
    out_reading->display_net = is_net;
    out_reading->raw_status = lamp_status;
    out_reading->raw_error = 0U;

    /* raw_status preserves the opaque octet; status comes from US/ST/OL. */
    return SCALE_REJ_NONE;
}

static bool ci2001_parse_frame_with_length(const uint8_t* frame, size_t length,
                                           scale_reading_t* out_reading)
{
    if (out_reading == NULL) return false;
    return ci2001_decode(frame, length, out_reading) == SCALE_REJ_NONE;
}

static scale_reject_reason_t ci2001_classify_reject(const uint8_t* frame, size_t length)
{
    return ci2001_decode(frame, length, NULL);
}

const scale_protocol_t cas_ci2001_protocol = {
    .name = "CAS_CI150A",
    .frame_end = "\r\n",
    .parse_frame = NULL,
    .parse_frame_with_length = ci2001_parse_frame_with_length,
    .build_poll_request = NULL,
    .classify_reject = ci2001_classify_reject,
};

bool cas_ci2001_parser_self_test(void)
{
    /* Body only: CR/LF has already been consumed by scale_reader. */
    const uint8_t valid_frame[CI2001_FRAME_BODY_SIZE] = {
        'S', 'T', ',', 'G', 'S', ',', 0x0AU, 0x00U, ',',
        '0', '0', '1', '0', '.', '0', '0', '0', 0x00U, 'k', 'g'
    };
    uint8_t malformed[CI2001_FRAME_BODY_SIZE];
    scale_reading_t reading;

    if (!ci2001_parse_frame_with_length(valid_frame, sizeof(valid_frame),
                                        &reading) ||
        !reading.valid || !reading.has_gross || !reading.has_display ||
        !reading.has_status || !reading.stable ||
        reading.gross != 10.0 || reading.display != 10.0 ||
        !reading.has_unit || strcmp(reading.unit, "kg") != 0 ||
        reading.raw_status != 0x00U) {
        return false;
    }

    memcpy(malformed, valid_frame, sizeof(malformed));
    malformed[2U] = (uint8_t)';';
    if (ci2001_parse_frame_with_length(malformed, sizeof(malformed), &reading)) {
        return false;
    }

    memcpy(malformed, valid_frame, sizeof(malformed));
    malformed[CI2001_WEIGHT_OFFSET + 2U] = (uint8_t)'X';
    if (ci2001_parse_frame_with_length(malformed, sizeof(malformed), &reading)) {
        return false;
    }
    return true;
}
