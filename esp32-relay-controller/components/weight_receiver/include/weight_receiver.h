#ifndef WEIGHT_RECEIVER_H
#define WEIGHT_RECEIVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Accepted weight window, in integer grams. Negatives and absurd values are
 * rejected; everything inside the window is accepted and drives the relay
 * through the trigger comparison below. */
#define WEIGHT_G_MIN 0
#define WEIGHT_G_MAX 100000 /* 100 kg */

/* Trigger point, centralized: WEIGHT_TRIGGER_G is the single definition of
 * the "10.0 kg" threshold, expressed in integer grams so the decision is an
 * integer >= comparison — never a float equality. Kconfig supplies the value
 * in the firmware build; the fallback keeps this component (and its QEMU test
 * project, which has no Kconfig) buildable standalone. */
#ifndef CONFIG_WEIGHT_DEMO_TRIGGER_G
#define CONFIG_WEIGHT_DEMO_TRIGGER_G 10000
#endif
#define WEIGHT_TRIGGER_G CONFIG_WEIGHT_DEMO_TRIGGER_G

/* Guarded so this component builds before its Kconfig entry exists; the value
 * comes from Kconfig once defined, and the default matches the spec. */
#ifndef CONFIG_WEIGHT_DEMO_MESSAGE_TIMEOUT_MS
#define CONFIG_WEIGHT_DEMO_MESSAGE_TIMEOUT_MS 5000
#endif
#define WEIGHT_MESSAGE_TIMEOUT_MS CONFIG_WEIGHT_DEMO_MESSAGE_TIMEOUT_MS

typedef struct {
    int32_t  weight_g;  /* authoritative integer grams (threshold input) */
    int      weight_kg; /* rounded convenience value: lround(g / 1000) */
    uint32_t sequence;
    bool     has_sequence;
    uint32_t age_ms;   /* acquisition age supplied by sender; legacy = zero */
    char     source[8];
    bool     has_source;
    /* Additive protocol fields from the simulator. Frames that omit them (all
     * real-scale frames, and every frame from a sender that predates them)
     * leave these false, so nothing that was accepted before is rejected now. */
    bool     simulated;
    bool     has_cycle;
    uint32_t simulation_cycle;
    bool     stable;
    bool     has_stable;
    bool     has_weight1;
    int32_t  weight1_g;
    bool     stable1;
    bool     has_stable1;
    bool     has_weight2;
    int32_t  weight2_g;
    bool     stable2;
    bool     has_stable2;
    /* True when the frame carried weight1_g or weight2_g at all, even as null.
     * Such a frame is per-channel: weight_g must never stand in for a channel. */
    bool     has_channel_fields;
    /* Single-scale identity (CONTRACT 9.11). The ONE sample type the dispense
     * controller consumes is {weight_g, stable, scale_id, boot_id, sequence, age_ms}.
     * weight_mqtt fills scale_id/boot_id; a legacy WS frame carries neither
     * (has_* false). The per-channel fields above are LEGACY WS parse output only:
     * the controller never reads them, so a frame is never attributed to a pump. */
    bool     has_scale_id;
    char     scale_id[17];
    bool     has_boot_id;
    char     boot_id[9];
} weight_msg_t;

typedef enum {
    WEIGHT_MSG_ACCEPTED,
    WEIGHT_MSG_REJECTED
} weight_msg_result_t;

typedef enum {
    WEIGHT_LINK_UNKNOWN,
    WEIGHT_LINK_OK,
    WEIGHT_LINK_TIMEOUT,
    WEIGHT_LINK_LOST
} weight_link_state_t;

/* Pure: parse and validate. No hardware, no state. Accepts weight_g (integer
 * grams, authoritative) and/or weight_kg (fractional allowed, converted to
 * grams by rounding). Rejects malformed JSON, non-finite or out-of-window
 * values, fractional weight_g, and anything missing both weight fields. */
weight_msg_result_t weight_msg_parse(const char *json, size_t len, weight_msg_t *out);

/* Pure: the bench rule — at or above the trigger energizes. Integer gram
 * comparison; there is no float equality anywhere in the decision path. */
bool weight_should_energize_g(int32_t weight_g);

/* Pure: missed messages between two sequence numbers; 0 on wrap or repeat. */
uint32_t weight_sequence_gap(uint32_t last, uint32_t current);

/* Pure: has the valid-message timestamp gone stale? */
bool weight_link_timed_out(uint32_t now_ms, uint32_t last_valid_ms, uint32_t timeout_ms);

esp_err_t weight_receiver_init(void);
void weight_receiver_on_message(const char *json, size_t len, uint32_t now_ms);
void weight_receiver_on_link_lost(uint32_t now_ms);
void weight_receiver_tick(uint32_t now_ms);
weight_link_state_t weight_receiver_link_state(void);
/* Stable wire name for telemetry: "UNKNOWN" | "OK" | "TIMEOUT" | "LOST".
 * The server's health panel maps this to the Weight Sender indicator. */
const char *weight_link_state_name(weight_link_state_t state);

/* ---------------------------------------------------------------------------
 * Actuation seam.
 *
 * This module owns everything that is true of ANY weight feed — parsing,
 * validation, sequence/freshness tracking, the link state, and the hard
 * failsafe that de-energizes every relay when the data stops. What to DO with
 * an accepted weight is policy, and policy differs between the legacy
 * level/group bench mode and the dispense controller.
 *
 * With no handler registered the built-in legacy behaviour runs (all 12 relays
 * as one group, >= WEIGHT_TRIGGER_G). Registering a handler replaces that
 * decision only; the failsafe above is unaffected either way.
 *
 * The handler runs in the WebSocket client task on every accepted message, so
 * it must not block.
 * ------------------------------------------------------------------------ */
typedef void (*weight_actuation_fn)(const weight_msg_t *msg, uint32_t now_ms);
void weight_receiver_set_actuation_handler(weight_actuation_fn fn);

/* Manual-relay failsafe exemption. When set to a non-zero channel (1 or 2),
 * the stale-weight failsafe spares that channel's relay while still forcing
 * every other relay OFF. Reset to 0 to restore full failsafe coverage.
 * Wi-Fi loss and E-Stop always force ALL relays off regardless of this. */
void weight_receiver_set_manual_channel(uint8_t channel);
uint8_t weight_receiver_manual_channel(void);

/* Latest ACCEPTED weight and the time it arrived. False while no valid message
 * is held. Safe to call from another task (the web API does). */
bool weight_receiver_get_latest(weight_msg_t *out, uint32_t *out_ms);
bool weight_receiver_have_valid(void);
uint32_t weight_receiver_last_rx_ms(void);
uint32_t weight_receiver_last_sequence(void);
bool weight_receiver_have_sequence(void);

#endif /* WEIGHT_RECEIVER_H */
