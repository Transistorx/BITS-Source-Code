#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define SCALE_UART_EVENT_QUEUE_SIZE 20

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a scale UART and return its driver event queue. The configured
 * ESP-IDF console UART is deliberately excluded from this API.
 */
esp_err_t uart_driver_init(uart_port_t port, int baud_rate, int rx_pin, int tx_pin,
                           QueueHandle_t *event_queue);

#ifdef __cplusplus
}
#endif

#endif // UART_DRIVER_H
