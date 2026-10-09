#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/*
 * Authoritative weighing-scale board pin mapping.
 *
 * This header is the ONLY place GPIO and UART-peripheral numbers for the
 * scale interface are defined. Nothing else in the firmware may hardcode a
 * scale UART pin; callers ask board_pinmap_get() and get one filled
 * scale_link_config_t.
 *
 * HARDWARE: RS-485 scale interface on MAX13487E AutoDirection transceivers.
 * The MAX13487E drives DE/RE from its own TX activity, so the ESP32 side is
 * an ordinary full-duplex UART:
 *   - NO separate DE/RE GPIO exists or is allocated
 *   - NO RTS pin is wired to direction control
 *   - UART_MODE_RS485_HALF_DUPLEX is NOT used and must not be enabled
 * Direction is entirely hardware-auto. The only software consequence is that
 * the transceiver may echo TX into RX, which the UART backend flushes.
 *
 * Board variant selection is a single Kconfig choice so the legacy RS-232
 * map stays selectable in the same commit (Phase 0 gate requirement).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "scale_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Identity
 * -------------------------------------------------------------------------- */

typedef enum {
    BOARD_CH_SCALE_1 = 0,
    BOARD_CH_SCALE_2 = 1,
    BOARD_CH_SCALE_COUNT = 2
} board_channel_t;

/*
 * DE/RE control mode. On this board it is always hardware-auto; the enum
 * exists so the startup diagnostic can say so explicitly and so a future
 * board with a software-controlled transceiver has somewhere to record it.
 */
typedef enum {
    BOARD_DE_RE_AUTO_DIRECTION = 0,  /* MAX13487E: no ESP32 pin involved */
    BOARD_DE_RE_NONE = 1,            /* RS-232: no direction control at all */
    BOARD_DE_RE_SOFTWARE = 2         /* reserved; NOT used on this board */
} board_de_re_mode_t;

/*
 * Connector / differential-pair naming.
 *
 * The RS-485 connector pinout (which screw terminal is A, which is B, which
 * is GND) is NOT proven by any repository file. These are recorded as
 * VERIFY ON PCB rather than guessed. Do not treat the strings as facts.
 */
typedef struct {
    const char *connector;  /* connector designator, e.g. "J-RS485-1" */
    const char *a_pin;      /* differential A / non-inverting */
    const char *b_pin;      /* differential B / inverting */
    const char *gnd_pin;    /* signal/common ground */
} board_rs485_connector_t;

typedef struct {
    board_channel_t channel;
    const char *label;              /* "RS485 CH1" or "RS232 CH1" */
    int uart_port;                  /* ESP-IDF UART peripheral number */
    int tx_gpio;
    int rx_gpio;
    int baud_rate;                  /* default only; Kconfig may override */
    scale_xport_type_t transport;
    board_de_re_mode_t de_re_mode;
    board_rs485_connector_t connector;
    bool rx_is_input_only_pad;      /* GPIO34/35/36/39 class */
    bool gpio15_strap_pin;          /* RX on a strapping pad */
} board_scale_link_t;

/* --------------------------------------------------------------------------
 * Board variant selection (one Kconfig choice)
 * -------------------------------------------------------------------------- */

#if defined(CONFIG_WEIGHT_DEMO_BOARD_RS485_MAX13487E)
#define BOARD_VARIANT_NAME "RS485_MAX13487E"
#define BOARD_VARIANT_IS_RS485 1
#elif defined(CONFIG_WEIGHT_DEMO_BOARD_RS232_LEGACY)
#define BOARD_VARIANT_NAME "RS232_LEGACY"
#define BOARD_VARIANT_IS_RS485 0
#else
#error "No weighing-scale board variant selected. Choose one of CONFIG_WEIGHT_DEMO_BOARD_RS485_MAX13487E or CONFIG_WEIGHT_DEMO_BOARD_RS232_LEGACY."
#endif

/* --------------------------------------------------------------------------
 * Hardware map
 *
 * Single source of truth. Values come from the weighing-scale board
 * schematic (RS-485 revision, MAX13487E) and, for the legacy variant, from
 * the RS-232 PCB map recorded in task.txt / GOLDEN_BASELINE.md.
 * -------------------------------------------------------------------------- */

#if BOARD_VARIANT_IS_RS485

/* RS-485 Channel 1 — MAX13487E #1 */
#define BOARD_CH1_UART_PORT 1
#define BOARD_CH1_TX_GPIO   13   /* VFD_TX  */
#define BOARD_CH1_RX_GPIO   15   /* VFD_RX  — MTDO strapping pin */

/* RS-485 Channel 2 — MAX13487E #2 */
#define BOARD_CH2_UART_PORT 2
#define BOARD_CH2_TX_GPIO   32   /* VFD_TX_2 */
#define BOARD_CH2_RX_GPIO   33   /* VFD_RX_2 */

#define BOARD_DEFAULT_BAUD  9600

#else /* BOARD_VARIANT_IS_RS485 == 0 : legacy RS-232 PCB */

/* RS-232 Scale Channel 1 — nets 232TX1 / 232RX1 */
#define BOARD_CH1_UART_PORT 1
#define BOARD_CH1_TX_GPIO   33   /* 232TX1 */
#define BOARD_CH1_RX_GPIO   35   /* 232RX1 — input-only pad */

/* RS-232 Scale Channel 2 — nets 232TX2 / 232RX2 */
#define BOARD_CH2_UART_PORT 2
#define BOARD_CH2_TX_GPIO   32   /* 232TX2 */
#define BOARD_CH2_RX_GPIO   34   /* 232RX2 — input-only pad */

#define BOARD_DEFAULT_BAUD  9600

#endif /* BOARD_VARIANT_IS_RS485 */

/* --------------------------------------------------------------------------
 * Reserved pads — must never be claimed by the scale interface
 * -------------------------------------------------------------------------- */

/* UART0 console / flashing. GPIO1 = U0TX, GPIO3 = U0RX. */
#define BOARD_CONSOLE_UART_PORT 0
#define BOARD_CONSOLE_TX_GPIO   1
#define BOARD_CONSOLE_RX_GPIO   3

/*
 * Classic-ESP32 EMAC/RMII pads. The weighing-scale board has no Ethernet,
 * but if a pin is ever remapped onto one of these and EMAC is enabled the
 * build must fail rather than silently steal a MAC pad.
 */
#define BOARD_EMAC_PAD_COUNT 9
#define BOARD_EMAC_PADS { 17, 18, 19, 21, 22, 23, 25, 26, 27 }

/* Input-only pads on classic ESP32 (no output driver, no internal pull-up). */
#define BOARD_IS_INPUT_ONLY_PAD(g) ((g) >= 34 && (g) <= 39)

/* Strapping pads on classic ESP32. GPIO15 is MTDO and is the scale RX on
 * RS-485 Channel 1, so it can disturb boot if driven low during reset. */
#define BOARD_GPIO15_MTDO_STRAP 15

/* --------------------------------------------------------------------------
 * Compile-time guards
 *
 * These run unconditionally on every build of every board variant. They
 * reject an invalid TX/RX pair, console-UART theft, duplicate UART
 * ownership, input-only pads used as TX, and EMAC pad collisions.
 * -------------------------------------------------------------------------- */

/* --- per-channel TX/RX sanity --- */
_Static_assert(BOARD_CH1_TX_GPIO != BOARD_CH1_RX_GPIO,
               "Scale CH1: TX and RX cannot share a pad");
_Static_assert(BOARD_CH2_TX_GPIO != BOARD_CH2_RX_GPIO,
               "Scale CH2: TX and RX cannot share a pad");

/* --- UART peripheral ownership --- */
_Static_assert(BOARD_CH1_UART_PORT != BOARD_CH2_UART_PORT,
               "Scale CH1 and CH2 must not share a UART peripheral");
_Static_assert(BOARD_CH1_UART_PORT != BOARD_CONSOLE_UART_PORT,
               "Scale CH1 must not take the console/flashing UART0");
_Static_assert(BOARD_CH2_UART_PORT != BOARD_CONSOLE_UART_PORT,
               "Scale CH2 must not take the console/flashing UART0");
_Static_assert(BOARD_CH1_UART_PORT >= 1 && BOARD_CH1_UART_PORT <= 2,
               "Classic ESP32 has UART0..UART2 only");
_Static_assert(BOARD_CH2_UART_PORT >= 1 && BOARD_CH2_UART_PORT <= 2,
               "Classic ESP32 has UART0..UART2 only");

/* --- console UART pins must stay with the console --- */
_Static_assert(BOARD_CH1_TX_GPIO != BOARD_CONSOLE_TX_GPIO &&
               BOARD_CH1_TX_GPIO != BOARD_CONSOLE_RX_GPIO,
               "Scale CH1 TX collides with the UART0 console pin");
_Static_assert(BOARD_CH1_RX_GPIO != BOARD_CONSOLE_TX_GPIO &&
               BOARD_CH1_RX_GPIO != BOARD_CONSOLE_RX_GPIO,
               "Scale CH1 RX collides with the UART0 console pin");
_Static_assert(BOARD_CH2_TX_GPIO != BOARD_CONSOLE_TX_GPIO &&
               BOARD_CH2_TX_GPIO != BOARD_CONSOLE_RX_GPIO,
               "Scale CH2 TX collides with the UART0 console pin");
_Static_assert(BOARD_CH2_RX_GPIO != BOARD_CONSOLE_TX_GPIO &&
               BOARD_CH2_RX_GPIO != BOARD_CONSOLE_RX_GPIO,
               "Scale CH2 RX collides with the UART0 console pin");

/* --- TX must be an output-capable pad --- */
_Static_assert(!BOARD_IS_INPUT_ONLY_PAD(BOARD_CH1_TX_GPIO),
               "Scale CH1 TX is on an input-only pad (GPIO34-39 cannot transmit)");
_Static_assert(!BOARD_IS_INPUT_ONLY_PAD(BOARD_CH2_TX_GPIO),
               "Scale CH2 TX is on an input-only pad (GPIO34-39 cannot transmit)");

/* --- no pad may serve both channels --- */
_Static_assert(BOARD_CH1_TX_GPIO != BOARD_CH2_TX_GPIO &&
               BOARD_CH1_TX_GPIO != BOARD_CH2_RX_GPIO,
               "Scale CH1 TX pad is reused by Scale CH2");
_Static_assert(BOARD_CH1_RX_GPIO != BOARD_CH2_TX_GPIO &&
               BOARD_CH1_RX_GPIO != BOARD_CH2_RX_GPIO,
               "Scale CH1 RX pad is reused by Scale CH2");

/* --- EMAC/RMII pad collision --- */
_Static_assert(BOARD_CH1_TX_GPIO != 17 && BOARD_CH1_TX_GPIO != 18 &&
               BOARD_CH1_TX_GPIO != 19 && BOARD_CH1_TX_GPIO != 21 &&
               BOARD_CH1_TX_GPIO != 22 && BOARD_CH1_TX_GPIO != 23 &&
               BOARD_CH1_TX_GPIO != 25 && BOARD_CH1_TX_GPIO != 26 &&
               BOARD_CH1_TX_GPIO != 27,
               "Scale CH1 TX collides with an EMAC/RMII pad");
_Static_assert(BOARD_CH1_RX_GPIO != 17 && BOARD_CH1_RX_GPIO != 18 &&
               BOARD_CH1_RX_GPIO != 19 && BOARD_CH1_RX_GPIO != 21 &&
               BOARD_CH1_RX_GPIO != 22 && BOARD_CH1_RX_GPIO != 23 &&
               BOARD_CH1_RX_GPIO != 25 && BOARD_CH1_RX_GPIO != 26 &&
               BOARD_CH1_RX_GPIO != 27,
               "Scale CH1 RX collides with an EMAC/RMII pad");
_Static_assert(BOARD_CH2_TX_GPIO != 17 && BOARD_CH2_TX_GPIO != 18 &&
               BOARD_CH2_TX_GPIO != 19 && BOARD_CH2_TX_GPIO != 21 &&
               BOARD_CH2_TX_GPIO != 22 && BOARD_CH2_TX_GPIO != 23 &&
               BOARD_CH2_TX_GPIO != 25 && BOARD_CH2_TX_GPIO != 26 &&
               BOARD_CH2_TX_GPIO != 27,
               "Scale CH2 TX collides with an EMAC/RMII pad");
_Static_assert(BOARD_CH2_RX_GPIO != 17 && BOARD_CH2_RX_GPIO != 18 &&
               BOARD_CH2_RX_GPIO != 19 && BOARD_CH2_RX_GPIO != 21 &&
               BOARD_CH2_RX_GPIO != 22 && BOARD_CH2_RX_GPIO != 23 &&
               BOARD_CH2_RX_GPIO != 25 && BOARD_CH2_RX_GPIO != 26 &&
               BOARD_CH2_RX_GPIO != 27,
               "Scale CH2 RX collides with an EMAC/RMII pad");

/* --- MAX13487E AutoDirection: no DE/RE pad may be invented --- */
/*
 * If this ever trips, someone has reintroduced a software direction pin.
 * The MAX13487E owns direction internally; an ESP32 DE/RE pad on this board
 * is a wiring error, not a feature.
 */
#if defined(BOARD_HAS_DE_RE_GPIO)
#error "This board uses MAX13487E AutoDirection. There must be no ESP32 DE/RE GPIO."
#endif

/* --------------------------------------------------------------------------
 * Runtime API
 * -------------------------------------------------------------------------- */

/**
 * @brief  Look up the authoritative scale link definition for one channel.
 * @param  channel  BOARD_CH_SCALE_1 or BOARD_CH_SCALE_2.
 * @return Pointer to a static definition, or NULL for an invalid channel.
 */
const board_scale_link_t *board_pinmap_get(board_channel_t channel);

/**
 * @brief  The channel configured as the single physical scale input.
 * @return BOARD_CH_SCALE_1 or BOARD_CH_SCALE_2.
 */
board_channel_t board_pinmap_active_channel(void);

/** @brief The compiled (Kconfig) default for the active channel. */
board_channel_t board_pinmap_kconfig_channel(void);

/**
 * @brief  Override the active channel once at boot (NVS selection).
 *         Never called from a link-state path: no auto-switching.
 * @return false for an invalid channel (selection unchanged).
 */
bool board_pinmap_set_active_channel(board_channel_t channel);

/**
 * @brief  Fill a transport link config from the board map.
 * @param  channel  Which scale channel.
 * @param  baud_rate  Baud to install (Kconfig value; the map only defaults it).
 * @param  out  Receives uart_port / rx_gpio / tx_gpio / transport / channel.
 * @return true on success, false on a bad channel or NULL out.
 */
bool board_pinmap_fill_link(board_channel_t channel, int baud_rate,
                            scale_link_config_t *out);

/**
 * @brief  Startup diagnostic line for one channel.
 *
 * Example output:
 *   RS485 CH2 | UART2 | TX=GPIO32 | RX=GPIO33 | DE=AutoDirection
 */
void board_pinmap_log(board_channel_t channel);

/**
 * @brief  Startup diagnostic lines for every channel, plus the active one.
 */
void board_pinmap_log_all(void);

/**
 * @brief  Report the GPIO15 (MTDO) strapping-pad state at boot.
 *
 * On the RS-485 board Channel 1 RX sits on GPIO15. If that pad is low when
 * the ESP32 resets, boot behaviour changes (boot log silenced, and in the
 * worst case a device that drives the bus during reset can hold it low).
 * This reads the pad after bring-up and logs the finding; it never blocks
 * boot. Channel 2 (GPIO32/33) is the preferred single-scale channel when
 * Channel 1 shows boot instability.
 */
void board_pinmap_check_strap_pad(void);

/**
 * @brief  Human-readable DE/RE mode string for diagnostics.
 */
const char *board_de_re_mode_name(board_de_re_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PINS_H */
