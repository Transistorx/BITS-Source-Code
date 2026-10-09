#include "mock_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static mock_transport_t *self_of(scale_transport_t *t)
{
    return (mock_transport_t *)t->ctx;
}

static esp_err_t mock_open(scale_transport_t *t, const scale_link_config_t *cfg)
{
    mock_transport_t *self = self_of(t);
    if (self == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    self->open_calls++;
    self->opened = true;
    return ESP_OK;
}

static bool mock_wait_event(scale_transport_t *t, scale_xport_event_t *out,
                            uint32_t timeout_ms)
{
    mock_transport_t *self = self_of(t);
    if (self == NULL || out == NULL) return false;

    if (self->block_once_ms != 0U) {
        uint32_t stall = self->block_once_ms;
        self->block_once_ms = 0U;
        vTaskDelay(pdMS_TO_TICKS(stall));
        return false;
    }

    if (self->ev_len > 0U) {
        *out = self->events[0];
        memmove(&self->events[0], &self->events[1],
                (self->ev_len - 1U) * sizeof(self->events[0]));
        self->ev_len--;
        return true;
    }

    /*
     * Always honour the block, even when the link is not open. The contract
     * is "block up to timeout_ms"; returning instantly on an unopened link
     * would busy-loop the reader task at its priority and starve the test
     * task that is waiting to observe the result.
     */
    if (timeout_ms > 0U) vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    return false;
}

static int mock_read(scale_transport_t *t, uint8_t *buf, size_t max_len,
                     uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_transport_t *self = self_of(t);
    if (self == NULL || !self->opened || buf == NULL || max_len == 0U) return 0;

    self->read_calls++;
    if (self->fail_reads) return -1;

    size_t available = self->rx_len;
    if (available == 0U) return 0;

    size_t take = (available < max_len) ? available : max_len;
    memcpy(buf, self->rx, take);
    memmove(self->rx, self->rx + take, self->rx_len - take);
    self->rx_len -= take;
    self->bytes_served += (uint32_t)take;
    return (int)take;
}

static esp_err_t mock_flush_input(scale_transport_t *t)
{
    mock_transport_t *self = self_of(t);
    if (self == NULL || !self->opened) return ESP_ERR_INVALID_STATE;
    self->flush_calls++;
    self->rx_len = 0U;
    return ESP_OK;
}

static esp_err_t mock_get_buffered_len(scale_transport_t *t, size_t *out_len)
{
    mock_transport_t *self = self_of(t);
    if (out_len == NULL) return ESP_ERR_INVALID_ARG;
    if (self == NULL || !self->opened) {
        *out_len = 0U;
        return ESP_ERR_INVALID_STATE;
    }
    *out_len = self->rx_len;
    return ESP_OK;
}

static size_t mock_pending_events(scale_transport_t *t)
{
    mock_transport_t *self = self_of(t);
    if (self == NULL || !self->opened) return 0U;
    return self->ev_len;
}

static void mock_close(scale_transport_t *t)
{
    mock_transport_t *self = self_of(t);
    if (self == NULL || !self->opened) return;
    self->close_calls++;
    self->opened = false;
}

static const scale_transport_ops_t s_mock_ops = {
    .name = "MOCK",
    .open = mock_open,
    .wait_event = mock_wait_event,
    .read = mock_read,
    .flush_input = mock_flush_input,
    .get_buffered_len = mock_get_buffered_len,
    .pending_events = mock_pending_events,
    .close = mock_close,
};

void mock_transport_init(mock_transport_t *t)
{
    if (t == NULL) return;
    memset(t, 0, sizeof(*t));
    t->base.ops = &s_mock_ops;
    t->base.ctx = t;
}

bool mock_transport_push_bytes(mock_transport_t *t, const uint8_t *data,
                               size_t length)
{
    if (t == NULL || data == NULL || length == 0U) return false;
    if (t->rx_len + length > MOCK_XPORT_RX_CAP) return false;
    memcpy(t->rx + t->rx_len, data, length);
    t->rx_len += length;
    return true;
}

bool mock_transport_push_event(mock_transport_t *t, scale_xport_event_type_t type)
{
    if (t == NULL) return false;
    if (t->ev_len >= MOCK_XPORT_EV_CAP) return false;
    t->events[t->ev_len].type = type;
    t->ev_len++;
    return true;
}
