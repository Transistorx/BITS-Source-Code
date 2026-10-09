#include "safety_manager.h"

#include "esp_log.h"
#include "relay_driver.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_safety_mutex;

static const char *TAG = "RELAY_CTRL";

#ifndef CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS
#define CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS 3000
#endif
#define CONTROL_WATCHDOG_MS CONFIG_WEIGHT_DEMO_CONTROL_WATCHDOG_MS

static safety_fault_t s_fault = SAFETY_NONE;
static uint32_t s_fault_since_ms;
static uint32_t s_last_control_tick_ms;
static bool s_have_control_tick;
static bool s_estop_held;

const char *safety_fault_name(safety_fault_t fault)
{
    switch (fault) {
    case SAFETY_NONE:                return "NONE";
    case SAFETY_WEIGHT_LINK_LOST:    return "WEIGHT_LINK_LOST";
    case SAFETY_WEIGHT_STALE:        return "WEIGHT_STALE";
    case SAFETY_WEIGHT_DISCONNECTED: return "WEIGHT_DISCONNECTED";
    case SAFETY_OVERSHOOT:           return "OVERSHOOT";
    case SAFETY_TIMEOUT:             return "TIMEOUT";
    case SAFETY_INVALID_START:           return "INVALID_START";
    case SAFETY_EMERGENCY_STOP:      return "EMERGENCY_STOP";
    case SAFETY_CONTROL_FAILURE:     return "CONTROL_FAILURE";
    case SAFETY_NO_PROGRESS:         return "NO_PROGRESS";
    case SAFETY_SCALE_MOVED:         return "SCALE_MOVED";
    default:                         return "UNKNOWN";
    }
}

bool safety_fault_is_transient(safety_fault_t fault)
{
    return fault == SAFETY_WEIGHT_LINK_LOST ||
           fault == SAFETY_WEIGHT_STALE ||
           fault == SAFETY_WEIGHT_DISCONNECTED;
}

static esp_err_t safety_manager_init_impl(void)
{
    s_fault = SAFETY_NONE;
    s_fault_since_ms = 0U;
    s_last_control_tick_ms = 0U;
    s_have_control_tick = false;
    s_estop_held = false;
    /* Boot must never leave a solenoid energized, whatever the caller did
     * before this ran. */
    (void)relay_force_all_off();
    return ESP_OK;
}

static void safety_manager_raise_impl(safety_fault_t fault, uint32_t now_ms)
{
    if (fault == SAFETY_NONE) return;

    /* An emergency stop outranks every other fault: nothing may downgrade it
     * to something the operator can clear with a normal "clear". */
    if (s_estop_held && fault != SAFETY_EMERGENCY_STOP) {
        (void)relay_all_off();
        return;
    }

    if (s_fault == fault) {
        /* Already held. Re-assert the relays (cheap and idempotent — the
         * driver caches per channel) but do not re-log. */
        (void)relay_all_off();
        return;
    }

    bool first = (s_fault == SAFETY_NONE);
    s_fault = fault;
    s_fault_since_ms = now_ms;
    if (fault == SAFETY_EMERGENCY_STOP) s_estop_held = true;

    /* H1: the fault edge forces the OFF pattern onto the bus whatever the driver
     * thinks the relay state is (a failed ON may be latched). Re-asserts above
     * stay state-aware so a held fault does not write every tick. */
    (void)relay_force_all_off();

    ESP_LOGE(TAG, "SAFETY FAULT: %s%s - ALL RELAYS OFF",
             safety_fault_name(fault),
             first ? "" : " (replacing a held fault)");
}

static void safety_manager_on_valid_weight_impl(uint32_t now_ms)
{
    (void)now_ms;
    if (s_fault == SAFETY_NONE) return;
    if (!safety_fault_is_transient(s_fault)) return;

    ESP_LOGI(TAG, "Safety fault cleared on fresh weight: %s",
             safety_fault_name(s_fault));
    s_fault = SAFETY_NONE;
    s_fault_since_ms = 0U;
}

static bool safety_manager_clear_impl(void)
{
    if (s_estop_held) {
        ESP_LOGW(TAG, "Fault clear refused - emergency stop is held "
                      "(release it first)");
        return false;
    }
    if (s_fault == SAFETY_NONE) return true;

    ESP_LOGI(TAG, "Safety fault cleared by operator: %s",
             safety_fault_name(s_fault));
    s_fault = SAFETY_NONE;
    s_fault_since_ms = 0U;
    return true;
}

static bool safety_manager_release_estop_impl(void)
{
    if (!s_estop_held && s_fault != SAFETY_EMERGENCY_STOP) return true;

    ESP_LOGW(TAG, "Emergency stop released");
    s_estop_held = false;
    s_fault = SAFETY_NONE;
    s_fault_since_ms = 0U;
    (void)relay_all_off();
    return true;
}

static safety_fault_t safety_manager_fault_impl(void)
{
    return s_fault;
}

static bool safety_manager_fault_active_impl(void)
{
    return s_fault != SAFETY_NONE;
}

static uint32_t safety_manager_fault_since_ms_impl(void)
{
    return s_fault_since_ms;
}

static void safety_manager_note_control_tick_impl(uint32_t now_ms)
{
    s_last_control_tick_ms = now_ms;
    s_have_control_tick = true;
}

static uint32_t safety_manager_control_age_ms_impl(uint32_t now_ms)
{
    if (!s_have_control_tick) return 0U;
    return now_ms - s_last_control_tick_ms;
}

static void safety_manager_tick_impl(uint32_t now_ms)
{
    if (!s_have_control_tick) return;      /* controller not started yet */
    if (s_fault == SAFETY_EMERGENCY_STOP) return;

    /* An emergency stop is held on purpose; the watchdog must not overwrite
     * it with a CONTROL_FAILURE that a plain "clear" could then dismiss. */
    if (s_fault == SAFETY_CONTROL_FAILURE) return;

    if (safety_manager_control_age_ms(now_ms) > CONTROL_WATCHDOG_MS) {
        safety_manager_raise(SAFETY_CONTROL_FAILURE, now_ms);
    }
}
/* M5: a watchdog/panic reset gives no operator acknowledgement of whatever the
 * machine was doing, and the relays may have been energized when it hit. Decided
 * from an INJECTED reason so tests can drive every case; app_main passes
 * esp_reset_reason(). BROWNOUT is deliberately not latched (power event, not a
 * firmware hang). */
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "OTHER";
    }
}

bool safety_manager_reset_reason_is_abnormal(esp_reset_reason_t reason)
{
    return reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_WDT || reason == ESP_RST_PANIC;
}

bool safety_manager_note_boot_reset(esp_reset_reason_t reason, uint32_t now_ms)
{
    if (!safety_manager_reset_reason_is_abnormal(reason)) return false;
    ESP_LOGE(TAG, "BOOT after %s reset - latching CONTROL_FAILURE until the "
                  "operator clears it", reset_reason_name(reason));
    safety_manager_raise(SAFETY_CONTROL_FAILURE, now_ms);
    return true;
}
/* Serialized fault state; no network work is permitted under this mutex. */
esp_err_t safety_manager_init(void)
{
    if(!s_safety_mutex) s_safety_mutex=xSemaphoreCreateRecursiveMutex();
    if(!s_safety_mutex) return ESP_ERR_NO_MEM;
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    esp_err_t result=safety_manager_init_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

void safety_manager_raise(safety_fault_t fault, uint32_t now_ms)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    safety_manager_raise_impl(fault, now_ms);
    xSemaphoreGiveRecursive(s_safety_mutex);
}

void safety_manager_on_valid_weight(uint32_t now_ms)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    safety_manager_on_valid_weight_impl(now_ms);
    xSemaphoreGiveRecursive(s_safety_mutex);
}

bool safety_manager_clear(void)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    bool result=safety_manager_clear_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

bool safety_manager_release_estop(void)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    bool result=safety_manager_release_estop_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

safety_fault_t safety_manager_fault(void)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    safety_fault_t result=safety_manager_fault_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

bool safety_manager_fault_active(void)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    bool result=safety_manager_fault_active_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

uint32_t safety_manager_fault_since_ms(void)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    uint32_t result=safety_manager_fault_since_ms_impl();
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

void safety_manager_note_control_tick(uint32_t now_ms)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    safety_manager_note_control_tick_impl(now_ms);
    xSemaphoreGiveRecursive(s_safety_mutex);
}

uint32_t safety_manager_control_age_ms(uint32_t now_ms)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    uint32_t result=safety_manager_control_age_ms_impl(now_ms);
    xSemaphoreGiveRecursive(s_safety_mutex);
    return result;
}

void safety_manager_tick(uint32_t now_ms)
{
    configASSERT(s_safety_mutex);
    xSemaphoreTakeRecursive(s_safety_mutex,portMAX_DELAY);
    safety_manager_tick_impl(now_ms);
    xSemaphoreGiveRecursive(s_safety_mutex);
}
