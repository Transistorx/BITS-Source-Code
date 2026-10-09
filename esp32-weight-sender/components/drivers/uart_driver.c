#include "uart_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "RS485";

#define SCALE_UART_RX_BUFFER_SIZE 1024

esp_err_t uart_driver_init(uart_port_t port, int baud_rate, int rx_pin, int tx_pin,
                           QueueHandle_t *event_queue)
{
    if (event_queue == NULL || port < 0 || port >= UART_NUM_MAX ||
        baud_rate <= 0 || rx_pin < 0 || tx_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *event_queue = NULL;

#if CONFIG_ESP_CONSOLE_UART
    /* Never install, flush, or reconfigure the console/programming UART. */
    if (port == CONFIG_ESP_CONSOLE_UART_NUM) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    const uart_config_t uart_cfg = {
        .baud_rate  = baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /*
     * At 9600 baud, 8N1, a continuously transmitting scale produces at most
     * about 960 bytes/s. A 1024-byte driver ring therefore absorbs over one
     * second of scheduling latency; the reader drains it every 20 ms.
     */
    esp_err_t err = uart_param_config(port, &uart_cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(port, tx_pin, rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    /*
     * The ESP32-side RX is a logic-level output from the RS232 receiver. The
     * pull-up provides an idle-high state when the transceiver is not fitted
     * or its output is high impedance.
     *
     * This PCB uses GPIO34/GPIO35 for RX, which are input-only pads with no
     * internal pull-up. gpio_set_pull_mode() returns ESP_OK for them anyway
     * because it discards the inner gpio_pullup_en() error, so the capability
     * is checked explicitly rather than trusting that return value: otherwise
     * the bias would be silently absent.
     */
    if (GPIO_IS_VALID_OUTPUT_GPIO(rx_pin)) {
        err = gpio_set_pull_mode((gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
        if (err != ESP_OK) return err;
    } else {
        ESP_LOGW(TAG, "RX GPIO%d is input-only and has no internal pull-up; "
                      "idle-high must come from the RS-232 receiver",
                 rx_pin);
    }

    err = uart_driver_install(port, SCALE_UART_RX_BUFFER_SIZE, 0,
                              SCALE_UART_EVENT_QUEUE_SIZE, event_queue, 0);
    if (err != ESP_OK) return err;

    if (*event_queue == NULL) {
        (void)uart_driver_delete(port);
        return ESP_FAIL;
    }
    return ESP_OK;
}
