#ifndef GOLDEN_FRAMES_H
#define GOLDEN_FRAMES_H

/*
 * Frozen RS232 golden vectors — captured BEFORE the transport abstraction.
 * See GOLDEN_BASELINE.md alongside this file.
 *
 * These frames are the wire form the CI-150A / CI-2001 emits in Stream Mode:
 * 20-byte body + CR + LF = 22 bytes. Tests replay them through the framing
 * layer and the CAS parser and assert the exact outputs recorded in the
 * baseline, so a transport refactor that changes any observable behaviour
 * fails here.
 *
 * DO NOT "fix" a golden frame to make a test pass. If a frame no longer
 * parses as recorded, the refactor changed behaviour.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wire frame helpers -------------------------------------------------
 * Body layout (20 bytes):
 *   [0-1] status  [2] ','  [3-4] GS|NT  [5] ','  [6] device id
 *   [7] lamp octet  [8] ','  [9-16] weight (8, right-aligned)
 *   [17] pad  [18-19] unit
 * Wire form appends CR LF.
 */

#define GOLDEN_BODY_SIZE 20U
#define GOLDEN_WIRE_SIZE 22U

typedef struct {
    const char *name;
    uint8_t wire[GOLDEN_WIRE_SIZE];   /* body + CR + LF */
    /* Expected parser output (scale_reading_t fields that must match). */
    bool expect_valid;
    bool expect_has_gross;
    bool expect_has_net;
    bool expect_has_display;
    bool expect_stable;
    bool expect_overload;
    double expect_gross;
    double expect_net;
    double expect_display;
    const char *expect_unit;
    uint32_t expect_raw_status;
    uint8_t expect_device_id;
} golden_frame_t;

/*
 * Weight field is 8 bytes right-aligned. Helper that packs " 0010.000"-style
 * text is deliberately avoided: each golden frame spells its bytes out so a
 * reviewer can diff them against the manual without running a generator.
 */

static const golden_frame_t k_golden_frames[] = {
    /* 1. Stable gross 10.000 kg, device id 1, lamp 0x00.
     *    Matches cas_ci2001_parser_self_test() shape with id=1. */
    {
        .name = "stable_gross_10kg",
        .wire = {
            'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
            '0','0','1','0','.','0','0','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = 10.0, .expect_net = 0.0, .expect_display = 10.0,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 2. Unstable gross 5.500 kg. */
    {
        .name = "unstable_gross_5_5kg",
        .wire = {
            'U','S', ',', 'G','S', ',', 0x01, 0x00, ',',
            '0','0','0','5','.','5','0','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = false, .expect_overload = false,
        .expect_gross = 5.5, .expect_net = 0.0, .expect_display = 5.5,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 3. Overload 9999.999 kg. */
    {
        .name = "overload_9999_999kg",
        .wire = {
            'O','L', ',', 'G','S', ',', 0x01, 0x00, ',',
            '9','9','9','9','.','9','9','9', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = false, .expect_overload = true,
        .expect_gross = 9999.999, .expect_net = 0.0, .expect_display = 9999.999,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 4. Stable net 0.000 kg (zero net, still a valid reading). */
    {
        .name = "stable_net_zero",
        .wire = {
            'S','T', ',', 'N','T', ',', 0x01, 0x00, ',',
            '0','0','0','0','.','0','0','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = false, .expect_has_net = true,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = 0.0, .expect_net = 0.0, .expect_display = 0.0,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 5. Device ID 10 = 0x0A = LF. Must survive the framing layer intact.
     *    This is the ci2001_device_id_is_lf() regression case. */
    {
        .name = "device_id_10_lf_collision",
        .wire = {
            'S','T', ',', 'G','S', ',', 0x0A, 0x00, ',',
            '0','0','1','0','.','0','0','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = 10.0, .expect_net = 0.0, .expect_display = 10.0,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 10U,
    },

    /* 6. Negative weight -1.500 kg (leading minus, right-aligned). */
    {
        .name = "negative_weight",
        .wire = {
            'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
            '-','0','0','1','.','5','0','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = -1.5, .expect_net = 0.0, .expect_display = -1.5,
        .expect_unit = "kg", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 7. lb unit. */
    {
        .name = "pound_unit",
        .wire = {
            'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
            '0','0','1','0','.','0','0','0', 0x00, 'l','b',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = 10.0, .expect_net = 0.0, .expect_display = 10.0,
        .expect_unit = "lb", .expect_raw_status = 0x00U, .expect_device_id = 1U,
    },

    /* 8. Opaque lamp octet 0x2A must be preserved in raw_status. */
    {
        .name = "opaque_lamp_octet",
        .wire = {
            'S','T', ',', 'G','S', ',', 0x01, 0x2A, ',',
            '0','0','0','7','.','2','5','0', 0x00, 'k','g',
            '\r', '\n'
        },
        .expect_valid = true,
        .expect_has_gross = true, .expect_has_net = false,
        .expect_has_display = true,
        .expect_stable = true, .expect_overload = false,
        .expect_gross = 7.25, .expect_net = 0.0, .expect_display = 7.25,
        .expect_unit = "kg", .expect_raw_status = 0x2AU, .expect_device_id = 1U,
    },
};

#define GOLDEN_FRAME_COUNT (sizeof(k_golden_frames) / sizeof(k_golden_frames[0]))

/* ---- Frozen malformed inputs: every one must stay rejected -------------- */

/* Wrong body length (19 bytes, one short). */
static const uint8_t k_golden_bad_short_body[19] = {
    'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'k'
};

/* Comma at offset 2 replaced with ';'. */
static const uint8_t k_golden_bad_comma2[GOLDEN_BODY_SIZE] = {
    'S','T', ';', 'G','S', ',', 0x01, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'k','g'
};

/* Status "XX" is not US/ST/OL. */
static const uint8_t k_golden_bad_status[GOLDEN_BODY_SIZE] = {
    'X','X', ',', 'G','S', ',', 0x01, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'k','g'
};

/* Gross/net "ZZ" is not GS/NT. */
static const uint8_t k_golden_bad_grossnet[GOLDEN_BODY_SIZE] = {
    'S','T', ',', 'Z','Z', ',', 0x01, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'k','g'
};

/* Device ID 100 > 99. */
static const uint8_t k_golden_bad_device_id[GOLDEN_BODY_SIZE] = {
    'S','T', ',', 'G','S', ',', 100, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'k','g'
};

/* Unit "zz" is not kg/lb. */
static const uint8_t k_golden_bad_unit[GOLDEN_BODY_SIZE] = {
    'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
    '0','0','1','0','.','0','0','0', 0x00, 'z','z'
};

/* Weight field contains 'X' where a digit belongs. */
static const uint8_t k_golden_bad_weight_char[GOLDEN_BODY_SIZE] = {
    'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
    '0','0','1','X','.','0','0','0', 0x00, 'k','g'
};

/* Weight field is all spaces — no digit. */
static const uint8_t k_golden_bad_weight_empty[GOLDEN_BODY_SIZE] = {
    'S','T', ',', 'G','S', ',', 0x01, 0x00, ',',
    ' ',' ',' ',' ',' ',' ',' ',' ', 0x00, 'k','g'
};

#ifdef __cplusplus
}
#endif

#endif /* GOLDEN_FRAMES_H */
