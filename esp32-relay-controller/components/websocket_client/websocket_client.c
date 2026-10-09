#include "websocket_client.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "weight_receiver.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "RELAY_CTRL";

/* Kconfig supplies these in the firmware build. The fallbacks keep this
 * component (and its QEMU test project, which has no Kconfig) buildable
 * standalone, and match the documented defaults. */
#ifndef CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_MDNS_NAME
#define CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_MDNS_NAME "weight-sender"
#endif
#ifndef CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_IP
#define CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_IP ""
#endif
#ifndef CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_PORT
#define CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_PORT 80
#endif
#ifndef CONFIG_WEIGHT_DEMO_WS_PATH
#define CONFIG_WEIGHT_DEMO_WS_PATH "/ws"
#endif

/* Upstream control frames are tiny (the valve state); the bound exists so a
 * bug cannot hand a huge buffer to the transport. */
#define WS_TX_MAX_LEN 256U

#define WS_OPCODE_CONTINUATION 0x00
#define WS_OPCODE_TEXT         0x01
#define WS_OPCODE_CLOSE        0x08
#define WS_OPCODE_PING         0x09
#define WS_OPCODE_PONG         0x0A

static const char *ws_opcode_name(int op_code)
{
    switch (op_code) {
    case WS_OPCODE_CLOSE: return "CLOSE";
    case WS_OPCODE_PING:  return "PING";
    case WS_OPCODE_PONG:  return "PONG";
    case WS_OPCODE_TEXT:  return "TEXT";
    case WS_OPCODE_CONTINUATION: return "CONT";
    default:              return "UNKNOWN";
    }
}

static char s_rx_buffer[WS_RX_BUFFER_SIZE];
static size_t s_rx_length;
static bool s_rx_overflow;
static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
/* weight_transport: when MQTT is the selected weight source the WebSocket must
 * not feed the receiver (no merging), and its reconnect churn must not raise
 * link-loss for a feed it does not own. Default true = unchanged behaviour. */
static volatile bool s_feed_enabled = true;
void websocket_client_set_feed_enabled(bool enabled) { s_feed_enabled = enabled; }
/* First DATA event after each (re)connect logs at INFO as proof the data path
 * is live; later ones stay at DEBUG so the stream (up to the scale frame
 * rate, about 22 Hz) cannot flood. */
static volatile bool s_data_logged_since_connect;
/* Failed connect attempts since the last CONNECTED, and the current target
 * URI. A field incident showed the relay looking silent/dead at INFO while
 * auto-reconnect hammered an unreachable server: attempts are now visible
 * (1st + every 30th, same pattern as the Wi-Fi manager) and every line names
 * the target so a wrong address is instantly obvious. */
static volatile uint32_t s_connect_attempts;
static char s_target_uri[160] = "";

void ws_rx_reset(void)
{
    s_rx_length = 0U;
    s_rx_overflow = false;
    s_rx_buffer[0] = '\0';
}

bool ws_rx_accumulate(const char *fragment, size_t len,
                      size_t payload_len, size_t payload_offset)
{
    if (fragment == NULL) return false;

    /* A fragment that does not fit is truncated, never wrapped or overflowed,
     * and the frame is flagged so the truncated text cannot be mistaken for a
     * complete message. */
    size_t space = (WS_RX_BUFFER_SIZE - 1U) - s_rx_length;
    size_t take = (len < space) ? len : space;
    if (take < len) s_rx_overflow = true;

    if (take > 0U) {
        memcpy(&s_rx_buffer[s_rx_length], fragment, take);
        s_rx_length += take;
    }
    s_rx_buffer[s_rx_length] = '\0';

    if (s_rx_overflow) return true;

    return (payload_offset + len) >= payload_len;
}

const char *ws_rx_buffer(void) { return s_rx_buffer; }
size_t ws_rx_length(void) { return s_rx_length; }

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void on_ws_event(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    const esp_websocket_event_data_t *data =
        (const esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        s_data_logged_since_connect = false;
        s_connect_attempts = 0U;
        ESP_LOGI(TAG, "Weight WebSocket connected");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        /* Failsafe first: the relay must not stay energized on a dead link.
         * It runs on every attempt. Visibility: a real connected->disconnected
         * transition always logs; a never-connected retry chain logs its 1st
         * and every 30th attempt (rate-limited, mirrors the Wi-Fi manager) so
         * an unreachable server is never silent at INFO. */
        s_connect_attempts++;
        if (s_connected) {
            ESP_LOGW(TAG, "Weight WebSocket disconnected; retrying %s",
                     s_target_uri);
        } else if (s_connect_attempts == 1U ||
                   (s_connect_attempts % 30U) == 0U) {
            ESP_LOGW(TAG, "Weight WebSocket not connected (attempt %lu); target %s",
                     (unsigned long)s_connect_attempts, s_target_uri);
        }
        s_connected = false;
        if (s_feed_enabled) weight_receiver_on_link_lost(now_ms());
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL) break;
        /* esp_websocket_client dispatches DATA events for CONTROL frames too
         * (PING/PONG/CLOSE). They must never consume the once-per-connect
         * visibility latch: a len=0 PONG reply to our own keepalive once ate
         * the INFO slot and muted every real frame to DEBUG. Control frames
         * log at DEBUG with their name; the latch waits for actual payload. */
        if (data->op_code != WS_OPCODE_TEXT && data->op_code != WS_OPCODE_CONTINUATION) {
            ESP_LOGD(TAG, "WS control frame opcode=%s len=%d",
                     ws_opcode_name(data->op_code), data->payload_len);
            break;
        }
        /* 0x01 opens a text message, 0x00 continues it. */
        if (!s_data_logged_since_connect) {
            s_data_logged_since_connect = true;
            ESP_LOGI(TAG, "WS data received | len=%d", data->payload_len);
        } else {
            ESP_LOGD(TAG, "WS data received | len=%d", data->payload_len);
        }

        /* Reset only when a new message starts, so continuations append. */
        if (data->payload_offset == 0) ws_rx_reset();

        if (ws_rx_accumulate(data->data_ptr, data->data_len,
                             data->payload_len, data->payload_offset) && s_feed_enabled) {
            weight_receiver_on_message(ws_rx_buffer(), ws_rx_length(), now_ms());
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        /* Fires on every auto-reconnect attempt while the server is down. */
        ESP_LOGD(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

static bool resolve_server_host(char *buf, size_t cap)
{
    /* The configured static IP is the PRIMARY target: normal operation must
     * not depend on mDNS (the sender holds a static lease by configuration). */
    if (CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_IP[0] != '\0') {
        snprintf(buf, cap, "%s", CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_IP);
        return true;
    }

    /* Optional diagnostic path, used only when no static IP is configured. */
    char mdns_name[128];
    snprintf(mdns_name, sizeof(mdns_name), "%s.local",
             CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_MDNS_NAME);

    if (mdns_init() == ESP_OK) {
        esp_ip4_addr_t addr;
        if (mdns_query_a(mdns_name, 3000, &addr) == ESP_OK) {
            snprintf(buf, cap, IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS resolved -> %s (diagnostic path)", buf);
            return true;
        }
    }

    return false;
}

esp_err_t websocket_client_start(void)
{
    /* Not re-entrant: a live client must be shut down before starting again,
     * so a retry loop can never leak the previous handle. */
    if (s_client != NULL) return ESP_ERR_INVALID_STATE;

    char host[64];
    if (!resolve_server_host(host, sizeof(host))) {
        /* The caller retries and announces the retry loop once, so a failed
         * attempt stays at debug level. */
        ESP_LOGD(TAG, "no weight server address available");
        return ESP_ERR_NOT_FOUND;
    }

    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s", host,
             CONFIG_WEIGHT_DEMO_WEIGHT_SERVER_PORT, CONFIG_WEIGHT_DEMO_WS_PATH);
    snprintf(s_target_uri, sizeof(s_target_uri), "%s", uri);

    ESP_LOGI(TAG, "Connecting to weight sender %s", uri);

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        .buffer_size = WS_RX_BUFFER_SIZE,
        .disable_auto_reconnect = false,
        /* Liveness probe: the sender's httpd WS server auto-answers protocol
         * PINGs with PONG (esp_http_server/src/httpd_ws.c). A socket that is
         * open but mute (e.g. the sender dropped its fd after a failed send)
         * is detected within ~10 s: no PONG -> client tears down ->
         * DISCONNECTED event -> failsafe + bounded auto-reconnect -> fresh
         * handshake re-arms the sender. This kills the mute-latch class
         * permanently instead of relying on either side to notice. */
        .ping_interval_sec = 5,
        .pingpong_timeout_sec = 10,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) return ESP_FAIL;

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                 on_ws_event, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    return err;
}

bool websocket_client_is_connected(void)
{
    return s_connected;
}

bool websocket_client_send_text(const char *text, uint32_t timeout_ms)
{
    if (text == NULL || s_client == NULL || !s_connected) return false;

    size_t len = strlen(text);
    if (len == 0U || len > WS_TX_MAX_LEN) return false;

    int sent = esp_websocket_client_send_text(s_client, text, (int)len,
                                              pdMS_TO_TICKS(timeout_ms));
    if (sent < 0) {
        ESP_LOGW(TAG, "upstream send failed (%d bytes): %s", (int)len,
                 esp_err_to_name(sent));
        return false;
    }
    return true;
}

const char *websocket_client_target(void)
{
    return s_target_uri;
}