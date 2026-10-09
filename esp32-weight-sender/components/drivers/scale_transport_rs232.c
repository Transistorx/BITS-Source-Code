#include "scale_transport_rs232.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "uart_driver.h"

#include <string.h>

static const char *TAG = "RS485";

/*
 * Translate the ESP-IDF UART event into the transport-agnostic event the
 * reader understands. Every type the reader reacts to has a counterpart;
 * anything else maps to DATA so the reader still drains the ring rather than
 * stalling on an event it does not recognise.
 */
static scale_xport_event_type_t map_uart_event(uart_event_type_t type)
{
    switch (type) {
    case UART_DATA:        return SCALE_XPORT_EV_DATA;
    case UART_FIFO_OVF:    return SCALE_XPORT_EV_FIFO_OVF;
    case UART_BUFFER_FULL: return SCALE_XPORT_EV_BUFFER_FULL;
    case UART_BREAK:       return SCALE_XPORT_EV_BREAK;
    case UART_PARITY_ERR:  return SCALE_XPORT_EV_PARITY_ERR;
    case UART_FRAME_ERR:   return SCALE_XPORT_EV_FRAME_ERR;
    case UART_PATTERN_DET: return SCALE_XPORT_EV_PATTERN_DET;
    default:               return SCALE_XPORT_EV_DATA;
    }
}

static scale_transport_rs232_t *self_of(scale_transport_t *t)
{
    return (scale_transport_rs232_t *)t->ctx;
}

static esp_err_t rs232_open(scale_transport_t *t, const scale_link_config_t *cfg)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;

    /* Re-opening would leak the driver and its event queue. */
    if (self->open) return ESP_ERR_INVALID_STATE;

    esp_err_t err = uart_driver_init((uart_port_t)cfg->uart_port, cfg->baud_rate,
                                     cfg->rx_gpio, cfg->tx_gpio, &self->event_queue);
    if (err != ESP_OK) return err;

    self->uart_port = cfg->uart_port;
    self->open = true;

    /*
     * Direction control is not configured here. On the RS-485 board a
     * MAX13487E AutoDirection transceiver drives DE/RE from its own TX
     * activity, so the ESP32 keeps an ordinary UART: no RTS direction pin,
     * no UART_MODE_RS485_HALF_DUPLEX, no manual DE toggling. The only
     * software consequence is that the transceiver can echo TX into RX,
     * which is flushed at the end of each write path.
     */
    const char *xport = (cfg->transport == SCALE_XPORT_RS485_AUTO_DIR)
                            ? "RS485" : "RS232";
    const char *de = (cfg->transport == SCALE_XPORT_RS485_AUTO_DIR)
                         ? "AutoDirection" : "None";
    ESP_LOGI(TAG, "%s link open: UART%d RX GPIO%d TX GPIO%d @ %d baud | DE=%s",
             xport, cfg->uart_port, cfg->rx_gpio, cfg->tx_gpio,
             cfg->baud_rate, de);
    return ESP_OK;
}

static bool rs232_wait_event(scale_transport_t *t, scale_xport_event_t *out,
                             uint32_t timeout_ms)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || !self->open || out == NULL || self->event_queue == NULL) {
        return false;
    }

    uart_event_t event;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(self->event_queue, &event, ticks) != pdTRUE) {
        return false;
    }
    out->type = map_uart_event(event.type);
    return true;
}

static int rs232_read(scale_transport_t *t, uint8_t *buf, size_t max_len,
                      uint32_t timeout_ms)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || !self->open || buf == NULL || max_len == 0U) {
        return 0;
    }

    int length = uart_read_bytes((uart_port_t)self->uart_port, buf, max_len,
                                 pdMS_TO_TICKS(timeout_ms));
    /* uart_read_bytes returns a negative esp_err_t on failure. Preserve that
     * distinction so the reader can count transport read errors exactly as it
     * did before the abstraction. */
    return length;
}

static esp_err_t rs232_flush_input(scale_transport_t *t)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || !self->open) return ESP_ERR_INVALID_STATE;
    return uart_flush_input((uart_port_t)self->uart_port);
}

static esp_err_t rs232_get_buffered_len(scale_transport_t *t, size_t *out_len)
{
    scale_transport_rs232_t *self = self_of(t);
    if (out_len == NULL) return ESP_ERR_INVALID_ARG;
    *out_len = 0U;
    if (self == NULL || !self->open) return ESP_ERR_INVALID_STATE;

    size_t buffered = 0U;
    esp_err_t err = uart_get_buffered_data_len((uart_port_t)self->uart_port, &buffered);
    if (err != ESP_OK) return err;
    *out_len = buffered;
    return ESP_OK;
}

static size_t rs232_pending_events(scale_transport_t *t)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || !self->open || self->event_queue == NULL) return 0U;
    return (size_t)uxQueueMessagesWaiting(self->event_queue);
}

static void rs232_close(scale_transport_t *t)
{
    scale_transport_rs232_t *self = self_of(t);
    if (self == NULL || !self->open) return;

    /* The reader task is stopped before close is ever reached in production;
     * deleting the driver here is the clean shutdown path. */
    (void)uart_driver_delete((uart_port_t)self->uart_port);
    self->event_queue = NULL;
    self->open = false;
}

static const scale_transport_ops_t s_rs232_ops = {
    .name = "RS232",
    .open = rs232_open,
    .wait_event = rs232_wait_event,
    .read = rs232_read,
    .flush_input = rs232_flush_input,
    .get_buffered_len = rs232_get_buffered_len,
    .pending_events = rs232_pending_events,
    .close = rs232_close,
};

esp_err_t scale_transport_rs232_init(scale_transport_rs232_t *t,
                                     const scale_link_config_t *cfg)
{
    if (t == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->uart_port < 0 || cfg->uart_port >= UART_NUM_MAX ||
        cfg->baud_rate <= 0 || cfg->rx_gpio < 0 || cfg->tx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(t, 0, sizeof(*t));
    t->base.ops = &s_rs232_ops;
    t->base.ctx = t;
    t->uart_port = cfg->uart_port;

    /*
     * Bring the UART up here so app_main keeps a single call, and so the
     * historical uart_driver_init refusal of the console UART still applies.
     * The caller passes one scale_link_config_t filled from board_pins.h, so
     * pin values are never assembled from scattered call-site constants.
     */
    return scale_transport_open(&t->base, cfg);
}
