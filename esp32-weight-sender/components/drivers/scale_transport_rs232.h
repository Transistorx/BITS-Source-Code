#ifndef SCALE_TRANSPORT_RS232_H
#define SCALE_TRANSPORT_RS232_H

/*
 * RS232 backend for the scale transport contract.
 *
 * This is the production link today and the rollback target for the whole
 * migration. It wraps the existing uart_driver_init() bring-up unchanged:
 * same 8N1 config, same pins, same RX ring size, same refusal of the console
 * UART. Nothing here knows about RS485.
 *
 * The uart_event_t -> scale_xport_event_t translation lives here so that
 * scale_reader never includes driver/uart.h.
 */

#include "scale_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Handle for one RS232 scale link. Allocate one per channel and keep it
 * alive for as long as the reader task runs; the reader holds a pointer to
 * `base`.
 */
typedef struct {
    scale_transport_t base;      /* .ops and .ctx are wired by init */
    int uart_port;
    QueueHandle_t event_queue;   /* uart_event_t queue from uart_driver_init */
    bool open;
} scale_transport_rs232_t;

/*
 * Initialise the UART link exactly as uart_driver_init does today and wire
 * the transport ops onto `t`. Returns ESP_ERR_INVALID_STATE if the port is
 * the configured console UART, matching the historical uart_driver_init
 * refusal.
 *
 * `cfg` comes from board_pins.h (board_pinmap_fill_link) and carries the
 * physical-layer label as well as the pad/peripheral numbers. On the RS-485
 * board the ESP32 side is still an ordinary UART because the MAX13487E owns
 * bus direction in hardware - this call never enables
 * UART_MODE_RS485_HALF_DUPLEX and never assigns a DE/RE pad.
 */
esp_err_t scale_transport_rs232_init(scale_transport_rs232_t *t,
                                     const scale_link_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SCALE_TRANSPORT_RS232_H */
