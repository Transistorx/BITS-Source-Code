#ifndef WEIGHT_SOURCE_H
#define WEIGHT_SOURCE_H

#include "esp_err.h"
#include "scale_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEIGHT_CHANNEL_COUNT 2U

/* ---------------------------------------------------------------------------
 * Production weight source: PHYSICAL CAS SCALE ONLY.
 *
 * There is no runtime simulator in this firmware. If the CAS scale is silent,
 * malformed or stale, weight_source_get() returns WEIGHT_SOURCE_RESULT_NONE and
 * NOTHING is published. The relay controller's stale-weight failsafe is the
 * correct response; a substituted value would hide a real fault and could drive
 * a dispense.
 *
 * The pure helpers below (weight_normalize_g, weight_sim_*, weight_ramp_*) are
 * compiled ONLY when CONFIG_WEIGHT_DEMO_TEST_SIMULATION is defined, which
 * happens in the QEMU host-test project and never in production firmware.
 * ------------------------------------------------------------------------ */

/* How long a genuine CAS weight field may go unseen before the link is
 * considered STALE/OFFLINE. Independent of any simulation concept. */
#ifndef CONFIG_WEIGHT_DEMO_CAS_STALE_MS
#define CONFIG_WEIGHT_DEMO_CAS_STALE_MS 3000
#endif
#define WEIGHT_CAS_STALE_MS CONFIG_WEIGHT_DEMO_CAS_STALE_MS

/* How long without any valid frame at all before the link is OFFLINE. */
#ifndef CONFIG_WEIGHT_DEMO_CAS_OFFLINE_MS
#define CONFIG_WEIGHT_DEMO_CAS_OFFLINE_MS 10000
#endif
#define WEIGHT_CAS_OFFLINE_MS CONFIG_WEIGHT_DEMO_CAS_OFFLINE_MS

/* Serial diagnostics are rate-limited so they cannot starve UART RX. */
#ifndef CONFIG_WEIGHT_DEMO_CAS_LOG_PERIOD_MS
#define CONFIG_WEIGHT_DEMO_CAS_LOG_PERIOD_MS 1000
#endif
#define WEIGHT_CAS_LOG_PERIOD_MS CONFIG_WEIGHT_DEMO_CAS_LOG_PERIOD_MS

typedef struct {
    int     weight_kg;
    int     weight_g;
    bool    from_scale;
    uint8_t channel;
    bool    stable;
    /* Provenance: every production sample is real CAS RS232 data. */
    const char *source;      /* always "CAS_RS232" for accepted samples */
    uint32_t sequence;       /* CAS sample sequence, monotonic per boot */
    uint32_t stamp_ms;       /* when this sample was decoded */
    uint32_t age_ms;         /* stamp age at read time */
    /* Independent, fresh physical measurements. A bundle can contain either
     * or both channels; the legacy weight_g remains the most recent channel. */
    bool     channel1_valid;
    int32_t  channel1_weight_g;
    bool     channel1_stable;
    bool     channel2_valid;
    int32_t  channel2_weight_g;
    bool     channel2_stable;
} weight_sample_t;

typedef struct {
    bool     valid;
    uint32_t stamp_ms;
    uint8_t  channel;
    int      grams;
    bool     stable;
} weight_channel_snapshot_t;

/* Why weight_source_get() returned nothing, or where the sample came from. */
typedef enum {
    WEIGHT_SOURCE_RESULT_NONE = 0, /* no fresh CAS data available */
    WEIGHT_SOURCE_RESULT_REAL      /* decoded from a real CAS scale channel */
} weight_source_result_t;

/* CAS link state, derived only from genuinely received and parsed frames. */
typedef enum {
    CAS_LINK_WAITING = 0,  /* UART up, no valid frame yet */
    CAS_LINK_ONLINE,       /* valid CAS frame received recently */
    CAS_LINK_STALE,        /* had data, now older than WEIGHT_CAS_STALE_MS */
    CAS_LINK_OFFLINE       /* silent longer than WEIGHT_CAS_OFFLINE_MS */
} cas_link_state_t;

const char *cas_link_state_name(cas_link_state_t state);
/* WAITING / ONLINE / STALE / OFFLINE, for log lines. */
const char *cas_link_state_short(cas_link_state_t state);

/* ---- Pure helpers ---- */

bool weight_normalize_g(double value, const char *unit, int *out_grams);
int  weight_grams_to_kg(int grams);
bool weight_pick_most_recent(const weight_channel_snapshot_t *channels, size_t count,
                             uint32_t now_ms, uint32_t window_ms,
                             weight_sample_t *out);
size_t weight_build_json(char *buf, size_t cap,
                         const weight_sample_t *sample, uint32_t sequence);
bool weight_sample_from_reading(const scale_reading_t *reading, uint8_t channel,
                                weight_sample_t *out, bool *out_used_display);

/* Which scale channel feeds the weight, freshness and link state.
 * 1 = CH1, 2 = CH2. 0 = any channel (host tests only; production firmware
 * always selects explicitly at boot). A frame on the other channel never
 * updates the sample, the sequence, the timestamp or the link state, so
 * noise or a stray device there cannot make the link look healthy. There is
 * no automatic switching. */
void weight_source_select_channel(uint8_t channel);
uint8_t weight_source_selected_channel(void);

uint32_t weight_source_now_ms(void);
void weight_source_init_logging(void);

esp_err_t weight_source_start(void);

/* Fills one sample and reports where it came from. Only REAL CAS data is ever
 * returned; anything else is WEIGHT_SOURCE_RESULT_NONE. */
weight_source_result_t weight_source_get(weight_sample_t *out);

/* ---- CAS provenance / diagnostics ---- */

cas_link_state_t weight_source_cas_link_state(void);
uint32_t weight_source_last_valid_cas_ms(void);   /* 0 until first valid frame */
uint32_t weight_source_cas_sequence(void);
bool     weight_source_have_valid_cas(void);
/* Last real weight in grams, only while it is still fresh (within the STALE
 * window). False when never received, stale or offline: callers must show
 * "n/a", never a zero. */
bool     weight_source_last_weight_g(int32_t *grams);
/* Prints the CAS SCALE: state line ONLY on a genuine link-state transition
 * (WAITING -> ONLINE -> STALE -> OFFLINE). A steady state never repeats the
 * same serial line. Per-sample provenance is printed by
 * weight_source_log_cas_sample() once per new CAS sequence. */
void weight_source_log_cas_status(uint32_t now_ms);
void weight_source_log_cas_sample(const weight_sample_t *sample, uint32_t sequence);

/* True while the selected channel reports OL (overload) in a fresh status:
 * no weight is published. weight_source_overload_events() counts how many
 * times overload was entered since weight_source_start(). */
bool     weight_source_overload_active(void);
uint32_t weight_source_overload_events(void);

uint32_t weight_source_peek_sequence(void);
void weight_source_advance_sequence(void);

/* ---------------------------------------------------------------------------
 * TEST-ONLY simulator helpers.
 *
 * Compiled solely into the QEMU host-test project. Production firmware never
 * defines CONFIG_WEIGHT_DEMO_TEST_SIMULATION, so these symbols do not exist on
 * the device and cannot become a weight source.
 * ------------------------------------------------------------------------ */
#ifdef CONFIG_WEIGHT_DEMO_TEST_SIMULATION

#ifndef CONFIG_WEIGHT_DEMO_SIM_STATIC_G
#define CONFIG_WEIGHT_DEMO_SIM_STATIC_G 5000
#endif
#define WEIGHT_SIM_STATIC_G CONFIG_WEIGHT_DEMO_SIM_STATIC_G

#ifndef CONFIG_WEIGHT_DEMO_SIM_COARSE_RATE_G_S
#define CONFIG_WEIGHT_DEMO_SIM_COARSE_RATE_G_S 1200
#endif
#ifndef CONFIG_WEIGHT_DEMO_SIM_FINE_RATE_G_S
#define CONFIG_WEIGHT_DEMO_SIM_FINE_RATE_G_S 200
#endif
#define WEIGHT_SIM_COARSE_RATE_G_S CONFIG_WEIGHT_DEMO_SIM_COARSE_RATE_G_S
#define WEIGHT_SIM_FINE_RATE_G_S   CONFIG_WEIGHT_DEMO_SIM_FINE_RATE_G_S

#ifndef CONFIG_WEIGHT_DEMO_SIM_MAX_G
#define CONFIG_WEIGHT_DEMO_SIM_MAX_G 100000
#endif
#define WEIGHT_SIM_MAX_G CONFIG_WEIGHT_DEMO_SIM_MAX_G

#ifndef CONFIG_WEIGHT_DEMO_RAMP_STEP_G
#define CONFIG_WEIGHT_DEMO_RAMP_STEP_G 1000
#endif
#ifndef CONFIG_WEIGHT_DEMO_RAMP_STEP_MS
#define CONFIG_WEIGHT_DEMO_RAMP_STEP_MS 1000
#endif
#ifndef CONFIG_WEIGHT_DEMO_RAMP_TOP_G
#define CONFIG_WEIGHT_DEMO_RAMP_TOP_G 20000
#endif
#ifndef CONFIG_WEIGHT_DEMO_RAMP_HOLD_MS
#define CONFIG_WEIGHT_DEMO_RAMP_HOLD_MS 5000
#endif

#define WEIGHT_RAMP_STEP_G   CONFIG_WEIGHT_DEMO_RAMP_STEP_G
#define WEIGHT_RAMP_STEP_MS  CONFIG_WEIGHT_DEMO_RAMP_STEP_MS
#define WEIGHT_RAMP_TOP_G    CONFIG_WEIGHT_DEMO_RAMP_TOP_G
#define WEIGHT_RAMP_HOLD_MS  CONFIG_WEIGHT_DEMO_RAMP_HOLD_MS
#define WEIGHT_RAMP_STEPS    (WEIGHT_RAMP_TOP_G / WEIGHT_RAMP_STEP_G)
#define WEIGHT_RAMP_TOP_MS   (WEIGHT_RAMP_STEPS * WEIGHT_RAMP_STEP_MS)
#define WEIGHT_RAMP_CYCLE_MS (WEIGHT_RAMP_TOP_MS + WEIGHT_RAMP_HOLD_MS)

typedef struct {
    int32_t  weight_g;
    uint32_t cycle;
    bool     holding;
} weight_ramp_sample_t;

void weight_ramp_at(uint32_t elapsed_ms, weight_ramp_sample_t *out);
bool weight_ramp_is_reset(int32_t prev_g, int32_t next_g);
int32_t weight_sim_step_g(uint32_t step);
int32_t weight_sim_integrate_g(int32_t current_g, bool coarse, bool fine,
                               uint32_t dt_ms);

#endif /* CONFIG_WEIGHT_DEMO_TEST_SIMULATION */

#endif /* WEIGHT_SOURCE_H */
