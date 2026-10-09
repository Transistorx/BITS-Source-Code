#include "websocket_server.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "log_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

/* Centralized endpoint configuration (Kconfig in the firmware build; the
 * fallbacks keep the QEMU test project, which has no Kconfig, buildable and
 * match the documented defaults). */
#ifndef CONFIG_WEIGHT_DEMO_WS_PORT
#define CONFIG_WEIGHT_DEMO_WS_PORT 80
#endif
#ifndef CONFIG_WEIGHT_DEMO_WS_PATH
#define CONFIG_WEIGHT_DEMO_WS_PATH "/ws"
#endif
/* The post-handshake callback field on httpd_uri_t exists only when this
 * Kconfig option is compiled in (esp_http_server.h L477-483). The firmware
 * build enables it via sdkconfig.defaults; the QEMU test project has no such
 * entry, so the guard keeps both builds compiling against their own struct
 * layout — same pattern as the WS_PORT/PATH fallbacks above. */
#ifndef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#define CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT 0
#endif

static const char *TAG = "NET";

static httpd_handle_t s_server;
static int s_client_fd = -1;
static SemaphoreHandle_t s_lock;
/* One outstanding queued send at a time. Set under s_lock when a work item is
 * reserved, cleared by the work function; bounds the queue even though the
 * stream (up to the scale frame rate, about 22 Hz) against httpd-task speed
 * makes buildup implausible. */
static bool s_send_pending;
/* Broadcast-failure log limiters (transmit task only; zero-init == armed
 * false). A failed sample is retried every tick, so unthrottled these would
 * log at tick rate. */
#define WS_FAIL_LOG_INTERVAL_MS 5000U
static log_ratelimit_t s_alloc_rl;
static log_ratelimit_t s_queue_rl;
/* Inbound control frames (the relay controller publishes its valve state and
 * job boundaries back over this same link). Runs in the httpd task. */
static websocket_server_rx_fn s_rx_handler;

static void clear_client(int fd)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (fd < 0 || s_client_fd == fd) s_client_fd = -1;
        xSemaphoreGive(s_lock);
    }
}

/* Heap context for one queued frame send (single allocation, flexible tail). */
typedef struct {
    int fd;
    size_t len;
    char payload[];
} ws_send_ctx_t;

/*
 * Runs IN THE HTTPD TASK via httpd_queue_work — the only context from which
 * httpd_ws_send_frame_async may be called cross-task. The IDF contract
 * (esp_http_server.h): "This API should rarely be called directly, with an
 * exception of asynchronous send using httpd_queue_work." The implementation
 * performs two unserialized sess->send_fn writes (header, then payload); a
 * direct call from transmit_task raced the httpd task's own PONG writes to
 * the same socket, which is what failed the first broadcast and created the
 * zombie link this design replaces.
 */
static void ws_send_work_fn(void *arg)
{
    ws_send_ctx_t *ctx = (ws_send_ctx_t *)arg;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)ctx->payload,
        .len = ctx->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_server, ctx->fd, &frame);

    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_send_pending = false;
        xSemaphoreGive(s_lock);
    }

    if (err != ESP_OK) {
        /* WARN carries the esp_err code (the diagnostic line); the INFO line
         * is the single standard removal log per event, identical to the
         * verifier's, so connect/disconnect pairs always match in the log. */
        ESP_LOGW(TAG, "WebSocket send to fd %d failed: %s - closing socket",
                 ctx->fd, esp_err_to_name(err));
        clear_client(ctx->fd);
        ESP_LOGI(TAG, "WebSocket client disconnected | fd=%d", ctx->fd);
        /* Kill the socket, not just the fd tracking. Without this the session
         * stays open, httpd keeps auto-PONGing the peer's keepalives, the
         * peer's ping/pong health check never trips, and the link is a zombie:
         * "healthy" WebSocket, permanently silent app layer. trigger_close is
         * itself queue-based (httpd_sess.c: httpd_queue_work(httpd_sess_close)),
         * so calling it from here — inside the httpd task — is safe; the close
         * runs after this work function returns. The peer then sees a TCP
         * close, failsafes, auto-reconnects, and the fresh handshake
         * re-registers the fd. */
        (void)httpd_sess_trigger_close(s_server, ctx->fd);
    }

    free(ctx);
}

#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
/*
 * Runs IN THE HTTPD TASK immediately after the 101 handshake response.
 * On IDF v6.1 this callback is the ONLY reliable registration point:
 * httpd_uri.c L339-367 processes the WS upgrade itself and explicitly does
 * NOT call the uri->handler for the handshake request, and post-handshake
 * httpd_parse.c L793-826 invokes the handler only for data frames, PONGs, or
 * (when handle_ws_control_frames is set) control frames. A registry written
 * from the handler's GET branch therefore never sees a real client — the
 * root cause of the field silence.
 */
static esp_err_t ws_post_handshake_cb(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_client_fd = fd;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "WebSocket client connected | fd=%d", fd);
    return ESP_OK;
}
#endif /* CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* On v6.1 the real WS handshake never reaches this handler (see
         * ws_post_handshake_cb), so a GET arriving here is a plain
         * non-upgrade request — e.g. a browser probe of /ws. Reject it; it
         * must NOT register as a client. The old code registered whatever
         * socket made a GET here, which was both dead for real handshakes
         * and wrong for probes. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WS upgrade required");
        return ESP_FAIL;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        clear_client(httpd_req_to_sockfd(req));
        (void)httpd_sess_trigger_close(s_server, httpd_req_to_sockfd(req));
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        /* Belt-and-braces only: without handle_ws_control_frames the CLOSE
         * branch is not the guaranteed teardown signal on v6.1 (httpd marks
         * ws_close internally). The authoritative drop detection is the
         * httpd_ws_get_fd_info() verification in websocket_server_has_client();
         * this branch just keeps the registry fresh when it does fire. */
        int fd = httpd_req_to_sockfd(req);
        clear_client(fd);
        ESP_LOGI(TAG, "WebSocket client disconnected | fd=%d", fd);
    }

    /* Inbound control frame from the relay controller. The first
     * httpd_ws_recv_frame(len 0) read only the header and left frame.len as the
     * payload length; a second call with that length reads the body — the
     * documented two-call pattern in esp_http_server.h. */
    if (frame.type == HTTPD_WS_TYPE_TEXT && frame.len > 0U) {
        if (frame.len > WS_CMD_MAX_LEN) {
            /* The payload is already in the socket buffer. Reading it into a
             * small buffer would desynchronise the frame stream, and skipping
             * it would corrupt the NEXT frame, so the only safe response is to
             * drop the connection: the relay controller reconnects and the
             * sender's registry is cleaned up by has_client()'s verification. */
            int fd = httpd_req_to_sockfd(req);
            ESP_LOGW(TAG, "inbound frame too large (%u bytes, max %u) - closing fd %d",
                     (unsigned)frame.len, (unsigned)WS_CMD_MAX_LEN, fd);
            clear_client(fd);
            (void)httpd_sess_trigger_close(req->handle, fd);
            return ESP_FAIL;
        }

        uint8_t payload[WS_CMD_MAX_LEN + 1U];
        frame.payload = payload;

        esp_err_t rx_err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (rx_err != ESP_OK) {
            int fd = httpd_req_to_sockfd(req);
            ESP_LOGW(TAG, "inbound frame read failed: %s - closing fd %d",
                     esp_err_to_name(rx_err), fd);
            clear_client(fd);
            (void)httpd_sess_trigger_close(req->handle, fd);
            return rx_err;
        }

        payload[frame.len] = '\0';
        if (s_rx_handler != NULL) {
            s_rx_handler((const char *)payload, (size_t)frame.len);
        }
    }

    return ESP_OK;
}

void websocket_server_set_rx_handler(websocket_server_rx_fn handler)
{
    s_rx_handler = handler;
}

esp_err_t websocket_server_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WEIGHT_DEMO_WS_PORT;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t ws_uri = {
        .uri = CONFIG_WEIGHT_DEMO_WS_PATH,
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
        /* The only registration point that actually fires on IDF v6.1. */
        .ws_post_handshake_cb = ws_post_handshake_cb,
#endif
    };

    err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registering %s failed: %s", ws_uri.uri, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WebSocket server started on %s (port %d)",
             CONFIG_WEIGHT_DEMO_WS_PATH, CONFIG_WEIGHT_DEMO_WS_PORT);
    return ESP_OK;
}

/*
 * Queues one TEXT frame for transmission in the httpd task context.
 * Returns true when the frame was ACCEPTED FOR SEND (queued), not when it is
 * on the wire — the actual send result is logged by ws_send_work_fn on
 * failure. Callers treat true as "transmitted for sequence purposes".
 */
bool websocket_server_broadcast(const char *text)
{
    if (text == NULL || s_server == NULL || s_lock == NULL) return false;

    int fd = -1;
    bool slot_reserved = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        fd = s_client_fd;
        if (fd >= 0 && !s_send_pending) {
            s_send_pending = true;
            slot_reserved = true;
        }
        xSemaphoreGive(s_lock);
    }

    /* No client is the normal case before ESP32 #2 connects: skip the send
     * rather than blocking or failing. */
    if (fd < 0) return false;

    if (!slot_reserved) {
        ESP_LOGD(TAG, "previous send still queued; skipping this tick");
        return false;
    }

    size_t len = strlen(text);
    ws_send_ctx_t *ctx = malloc(sizeof(*ctx) + len + 1U);
    if (ctx == NULL) {
        uint32_t folded = 0U;
        if (log_ratelimit_event(&s_alloc_rl, (uint32_t)(esp_timer_get_time() / 1000LL),
                                WS_FAIL_LOG_INTERVAL_MS, &folded)) {
            ESP_LOGE(TAG, "send context allocation failed (%u event(s) since last line)",
                     (unsigned)folded);
        }
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_send_pending = false;
            xSemaphoreGive(s_lock);
        }
        return false;
    }
    ctx->fd = fd;
    ctx->len = len;
    memcpy(ctx->payload, text, len + 1U);

    esp_err_t err = httpd_queue_work(s_server, ws_send_work_fn, ctx);
    if (err != ESP_OK) {
        uint32_t folded = 0U;
        if (log_ratelimit_event(&s_queue_rl, (uint32_t)(esp_timer_get_time() / 1000LL),
                                WS_FAIL_LOG_INTERVAL_MS, &folded)) {
            ESP_LOGW(TAG, "httpd_queue_work failed: %s (%u event(s) since last line)",
                     esp_err_to_name(err), (unsigned)folded);
        }
        free(ctx);
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_send_pending = false;
            xSemaphoreGive(s_lock);
        }
        return false;
    }

    return true;
}

/*
 * Registry truth, verified continuously instead of by (dead) CLOSE events:
 * the polled httpd_ws_get_fd_info() returns HTTPD_WS_CLIENT_WEBSOCKET only
 * while the session's ws_handshake_done is set and ws_close is not
 * (httpd_ws.c L564-572). It is httpd_sess_get-based and benign to poll
 * cross-task once per transmit tick (polled every tick, 20 ms); worst case is one tick of staleness,
 * bounded by the queue_work send-failure path. On drop, the registry entry
 * is cleared but the socket is NOT trigger_close'd — httpd has already
 * closed (or is closing) it, and closing a stale fd could hit a recycled
 * one.
 */
bool websocket_server_has_client(void)
{
    if (s_lock == NULL) return false;

    int fd = -1;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        fd = s_client_fd;
        xSemaphoreGive(s_lock);
    }
    if (fd < 0) return false;

    /* No httpd (QEMU suite / pre-start): nothing can be verified or served. */
    if (s_server == NULL) return false;

    if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        return true;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_client_fd == fd) s_client_fd = -1;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "WebSocket client disconnected | fd=%d", fd);
    return false;
}
