#ifndef SAFETY_MANAGER_H
#define SAFETY_MANAGER_H

#include "esp_err.h"
#include "esp_system.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Central fault latch for the relay controller.
 *
 * Every path that can leave a solenoid energized when it should not — a dead
 * weight link, stale or invalid data, overshoot, a stuck job, an emergency
 * stop, or a controller that stopped ticking — funnels through here. Raising
 * any fault de-energizes every relay immediately and keeps the controller in
 * FAULT until the fault is cleared.
 *
 * CLEARING POLICY (deliberate, and the two halves differ on purpose):
 *   - WEIGHT_LINK_LOST / WEIGHT_STALE / WEIGHT_DISCONNECTED are TRANSIENT:
 *     they describe the link, not the machine. They auto-clear when a fresh
 *     valid weight arrives, so a bench run recovers without an operator.
 *   - OVERSHOOT / TIMEOUT / EMERGENCY_STOP / CONTROL_FAILURE are LATCHED:
 *     each one means the machine did something the operator needs to look at,
 *     and silently resuming a dispense into an over-filled vessel is exactly
 *     the failure this module exists to prevent. They need an explicit clear.
 * ------------------------------------------------------------------------ */

typedef enum {
    SAFETY_NONE = 0,
    SAFETY_WEIGHT_LINK_LOST,
    SAFETY_WEIGHT_STALE,
    SAFETY_WEIGHT_DISCONNECTED,
    SAFETY_OVERSHOOT,
    SAFETY_TIMEOUT,
    /* The job could not have dispensed correctly from where it started - it
     * began at or above its own target. LATCHED: reporting this as a TIMEOUT
     * (or as a success) would name the wrong problem, and the operator needs
     * to see the real one. */
    SAFETY_INVALID_START,
    SAFETY_EMERGENCY_STOP,
    SAFETY_CONTROL_FAILURE,
    /* Single-scale watchdogs (CONTRACT 9.11). Both LATCHED (not transient): a relay
     * was ON with no weight progress, or the scale moved/rebooted/vanished under a
     * running job. Only an explicit operator clear removes them; no auto-retry. */
    SAFETY_NO_PROGRESS,
    SAFETY_SCALE_MOVED
} safety_fault_t;

esp_err_t safety_manager_init(void);

/* M5: call once after safety_manager_init() with esp_reset_reason(). A watchdog
 * or panic reset (TASK_WDT, INT_WDT, WDT, PANIC) latches CONTROL_FAILURE, which
 * only an explicit operator clear removes, so a silent reboot cannot erase the
 * acknowledgement. Returns true if it latched. The reason is only logged: no
 * telemetry field carries it yet (needs a CONTRACT change, next batch). */
bool safety_manager_note_boot_reset(esp_reset_reason_t reason, uint32_t now_ms);
bool safety_manager_reset_reason_is_abnormal(esp_reset_reason_t reason);

/* Latches `fault` if no fault is held, and forces every relay OFF. Re-raising
 * a fault that is already held only refreshes the timestamp, so the 250 ms
 * supervisor tick cannot flood the log. */
void safety_manager_raise(safety_fault_t fault, uint32_t now_ms);

/* A fresh, valid weight arrived: clears the TRANSIENT faults only. */
void safety_manager_on_valid_weight(uint32_t now_ms);

/* Explicit operator clear. Refuses (returns false) while an emergency stop is
 * held — that one is released with safety_manager_release_estop(). */
bool safety_manager_clear(void);

/* Releases an emergency stop and returns to a clean state. */
bool safety_manager_release_estop(void);

safety_fault_t safety_manager_fault(void);
bool safety_manager_fault_active(void);
const char *safety_fault_name(safety_fault_t fault);

/* True for faults that clear themselves when the link recovers. */
bool safety_fault_is_transient(safety_fault_t fault);

/* ---- Control watchdog ----
 * The dispense controller stamps its heartbeat every tick. safety_manager_tick
 * runs in an independent task and raises CONTROL_FAILURE when the heartbeat
 * has gone stale, including when the supervisor is blocked. ESP-IDF's idle
 * task watchdog separately detects a core monopolized by a busy loop. */
/* PRE-06: the independent watchdog task must outrank the supervisor it watches,
 * so a supervisor that never blocks cannot starve it on a shared core. Shared
 * here so app_main asserts it at compile time and the QEMU suite at run time. */
#define SAFETY_SUPERVISOR_TASK_PRIORITY 5
#define SAFETY_WATCHDOG_TASK_PRIORITY   6

void safety_manager_note_control_tick(uint32_t now_ms);
void safety_manager_tick(uint32_t now_ms);

/* Milliseconds since the last control heartbeat (diagnostics / tests). */
uint32_t safety_manager_control_age_ms(uint32_t now_ms);

uint32_t safety_manager_fault_since_ms(void);

#endif /* SAFETY_MANAGER_H */
