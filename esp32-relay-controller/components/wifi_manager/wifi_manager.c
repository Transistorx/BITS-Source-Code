#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "led_status.h"
#include "nvs_config.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "RELAY_CTRL";

/* Kconfig supplies these in the firmware build. The fallbacks keep this
 * component (and its QEMU test project, which has no Kconfig) buildable
 * standalone (empty static IP = plain DHCP). */
#ifndef CONFIG_WEIGHT_DEMO_STATIC_IP
#define CONFIG_WEIGHT_DEMO_STATIC_IP ""
#endif
#ifndef CONFIG_WEIGHT_DEMO_GATEWAY
#define CONFIG_WEIGHT_DEMO_GATEWAY ""
#endif
#ifndef CONFIG_WEIGHT_DEMO_NETMASK
#define CONFIG_WEIGHT_DEMO_NETMASK ""
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static bool s_connected;
static bool s_started; /* true only when the STA came up with stored credentials */
static uint32_t s_disconnect_count;
static bool s_ever_connected;       /* first GOT_IP seen; gates the attempt-1 WARN */
static void (*s_link_lost_cb)(void);
static bool s_static_active;        /* static lease applied to the STA netif */
static uint32_t s_static_expected;  /* configured address, for post-connect verification */

/* Parses dotted-quad IPv4 into the address word layout esp_netif uses
 * (network byte order: first octet in the least-significant byte, matching
 * IP2STR/IPSTR). Self-contained so no lwIP-internal header is needed.
 * Returns false on any malformed input, including trailing garbage. */
static bool parse_ip4(const char *s, uint32_t *out)
{
    unsigned a = 0U, b = 0U, c = 0U, d = 0U;
    char extra = '\0';
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255U || b > 255U || c > 255U || d > 255U) return false;
    *out = ((uint32_t)a) | (b << 8) | (c << 16) | (d << 24);
    return true;
}

/* Applies the configured static lease BEFORE the radio connects: the DHCP
 * client is stopped first, so the interface never holds a DHCP lease alongside
 * the static address. An empty/invalid configuration degrades to DHCP with a
 * single warning. */
static void apply_static_ip(void)
{
    s_static_active = false;
    if (CONFIG_WEIGHT_DEMO_STATIC_IP[0] == '\0') return;

    esp_netif_ip_info_t info = {0};
    if (!parse_ip4(CONFIG_WEIGHT_DEMO_STATIC_IP, &info.ip.addr) ||
        !parse_ip4(CONFIG_WEIGHT_DEMO_GATEWAY, &info.gw.addr) ||
        !parse_ip4(CONFIG_WEIGHT_DEMO_NETMASK, &info.netmask.addr)) {
        ESP_LOGE(TAG, "Invalid static IP configuration - using DHCP");
        return;
    }
    uint32_t ip = info.ip.addr;

    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) err = ESP_OK;
    if (err == ESP_OK) err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Static IP failed: %s - using DHCP", esp_err_to_name(err));
        (void)esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    s_static_active = true;
    s_static_expected = ip;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_connected;
        s_connected = false;
        if (s_events != NULL) xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        led_status_set(LED_CONNECTING);

        /* Failsafe before reconnecting: the relay must not stay energized
         * because the network went away. Runs on every disconnect, logged or
         * not. */
        if (s_link_lost_cb != NULL) s_link_lost_cb();

        /* Rate-limited: a drop from connected always logs, a retrying station
         * logs the first failure and every 30th after that, so bad credentials
         * cannot flood the console. Before the FIRST association even that
         * first-failure line is suppressed: a single missed connect attempt at
         * boot is normal (the AP may not answer immediately) and used to dirty
         * every clean boot log. The every-30th heartbeat still fires, so a
         * genuinely failing association stays visible. */
        s_disconnect_count++;
        if (was_connected || (s_ever_connected && s_disconnect_count == 1U) ||
            (s_disconnect_count % 30U) == 0U) {
            ESP_LOGW(TAG, "Wi-Fi disconnected (attempt %lu), reconnecting",
                     (unsigned long)s_disconnect_count);
        } else {
            ESP_LOGD(TAG, "Wi-Fi connect attempt %lu failed (pre-association), retrying",
                     (unsigned long)s_disconnect_count);
        }

        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        (void)data;
        s_connected = true;
        s_ever_connected = true;
        s_disconnect_count = 0U;
        if (s_events != NULL) xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        led_status_set(LED_CONNECTED);

        /* Retrieve the ACTUAL interface address and verify it against the
         * configured static lease — never just print the config value. */
        esp_netif_ip_info_t actual = {0};
        if (esp_netif_get_ip_info(s_sta_netif, &actual) != ESP_OK) {
            const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
            actual.ip = event->ip_info.ip;
        }
        if (s_static_active) {
            ESP_LOGI(TAG, "Static IP configured: %s", CONFIG_WEIGHT_DEMO_STATIC_IP);
            if (actual.ip.addr != s_static_expected) {
                ESP_LOGW(TAG, "Interface IP " IPSTR " does not match the configured static IP",
                         IP2STR(&actual.ip));
            }
        }
        ESP_LOGI(TAG, "Wi-Fi connected | IP=" IPSTR, IP2STR(&actual.ip));
    }
}

esp_err_t wifi_manager_start(void)
{
    /* Credentials live in NVS, written by the provisioning portal (hold BOOT
     * for 3 s) — the same flow as the UART weighing-scale gateway. */
    char ssid[33] = {0};
    char pass[65] = {0};
    (void)nvs_config_get_str(NVS_KEY_WIFI_SSID_1, ssid, sizeof(ssid));
    (void)nvs_config_get_str(NVS_KEY_WIFI_PASS_1, pass, sizeof(pass));

    /* netif/event loop always come up so the rest of the app can start even
     * while the device is unprovisioned and has no radio running. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;

    if (ssid[0] == '\0') {
        /* Skip Wi-Fi entirely instead of spinning on connect retries with an
         * empty SSID. Fast-blink as the "needs provisioning" hint. */
        ESP_LOGW(TAG, "No Wi-Fi credentials in NVS - Wi-Fi skipped. "
                      "Hold BOOT (GPIO0) for 3 s to open the setup portal.");
        led_status_set(LED_PROVISIONING);
        return ESP_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) return ESP_FAIL;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode =
        (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Static lease before the radio connects; DHCP client stays stopped. */
    apply_static_ip();

    led_status_set(LED_CONNECTING);
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    return ESP_OK;
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL || !s_started) return false;

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_started(void)
{
    return s_started;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

bool wifi_manager_get_ip(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0U || s_sta_netif == NULL) return false;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) return false;
    if (ip.ip.addr == 0U) return false;

    snprintf(buf, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

void wifi_manager_set_link_lost_callback(void (*cb)(void))
{
    s_link_lost_cb = cb;
}
