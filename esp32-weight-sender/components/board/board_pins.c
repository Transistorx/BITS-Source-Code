#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "BOARD";

/*
 * Connector and differential-pair designators are NOT proven by any
 * repository file. They are recorded as VERIFY ON PCB rather than guessed so
 * nobody mistakes an unverified string for a wiring instruction.
 */
#define CONNECTOR_UNVERIFIED "VERIFY ON PCB"

#if BOARD_VARIANT_IS_RS485
static const board_scale_link_t s_links[BOARD_CH_SCALE_COUNT] = {
    {
        .channel = BOARD_CH_SCALE_1,
        .label = "RS485 CH1",
        .uart_port = BOARD_CH1_UART_PORT,
        .tx_gpio = BOARD_CH1_TX_GPIO,
        .rx_gpio = BOARD_CH1_RX_GPIO,
        .baud_rate = BOARD_DEFAULT_BAUD,
        .transport = SCALE_XPORT_RS485_AUTO_DIR,
        .de_re_mode = BOARD_DE_RE_AUTO_DIRECTION,
        .connector = {
            .connector = CONNECTOR_UNVERIFIED,
            .a_pin = CONNECTOR_UNVERIFIED,
            .b_pin = CONNECTOR_UNVERIFIED,
            .gnd_pin = CONNECTOR_UNVERIFIED,
        },
        .rx_is_input_only_pad = false,
        .gpio15_strap_pin = (BOARD_CH1_RX_GPIO == BOARD_GPIO15_MTDO_STRAP),
    },
    {
        .channel = BOARD_CH_SCALE_2,
        .label = "RS485 CH2",
        .uart_port = BOARD_CH2_UART_PORT,
        .tx_gpio = BOARD_CH2_TX_GPIO,
        .rx_gpio = BOARD_CH2_RX_GPIO,
        .baud_rate = BOARD_DEFAULT_BAUD,
        .transport = SCALE_XPORT_RS485_AUTO_DIR,
        .de_re_mode = BOARD_DE_RE_AUTO_DIRECTION,
        .connector = {
            .connector = CONNECTOR_UNVERIFIED,
            .a_pin = CONNECTOR_UNVERIFIED,
            .b_pin = CONNECTOR_UNVERIFIED,
            .gnd_pin = CONNECTOR_UNVERIFIED,
        },
        .rx_is_input_only_pad = false,
        .gpio15_strap_pin = false,
    },
};
#else
static const board_scale_link_t s_links[BOARD_CH_SCALE_COUNT] = {
    {
        .channel = BOARD_CH_SCALE_1,
        .label = "RS232 CH1",
        .uart_port = BOARD_CH1_UART_PORT,
        .tx_gpio = BOARD_CH1_TX_GPIO,
        .rx_gpio = BOARD_CH1_RX_GPIO,
        .baud_rate = BOARD_DEFAULT_BAUD,
        .transport = SCALE_XPORT_RS232,
        .de_re_mode = BOARD_DE_RE_NONE,
        .connector = {
            .connector = CONNECTOR_UNVERIFIED,
            .a_pin = CONNECTOR_UNVERIFIED,
            .b_pin = CONNECTOR_UNVERIFIED,
            .gnd_pin = CONNECTOR_UNVERIFIED,
        },
        .rx_is_input_only_pad = true,
        .gpio15_strap_pin = false,
    },
    {
        .channel = BOARD_CH_SCALE_2,
        .label = "RS232 CH2",
        .uart_port = BOARD_CH2_UART_PORT,
        .tx_gpio = BOARD_CH2_TX_GPIO,
        .rx_gpio = BOARD_CH2_RX_GPIO,
        .baud_rate = BOARD_DEFAULT_BAUD,
        .transport = SCALE_XPORT_RS232,
        .de_re_mode = BOARD_DE_RE_NONE,
        .connector = {
            .connector = CONNECTOR_UNVERIFIED,
            .a_pin = CONNECTOR_UNVERIFIED,
            .b_pin = CONNECTOR_UNVERIFIED,
            .gnd_pin = CONNECTOR_UNVERIFIED,
        },
        .rx_is_input_only_pad = true,
        .gpio15_strap_pin = false,
    },
};
#endif

const char *board_de_re_mode_name(board_de_re_mode_t mode)
{
    switch (mode) {
    case BOARD_DE_RE_AUTO_DIRECTION: return "AutoDirection";
    case BOARD_DE_RE_NONE:           return "None";
    case BOARD_DE_RE_SOFTWARE:       return "Software";
    default:                         return "Unknown";
    }
}

const board_scale_link_t *board_pinmap_get(board_channel_t channel)
{
    if ((int)channel < 0 || (int)channel >= (int)BOARD_CH_SCALE_COUNT) {
        return NULL;
    }
    return &s_links[channel];
}

/*
 * Explicit channel selection. The compiled default comes from Kconfig
 * (WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL) and app_main may override it from NVS
 * once at boot via board_pinmap_set_active_channel(). It is NEVER changed at
 * run time by link state: there is no auto-switching between channels.
 */
board_channel_t board_pinmap_kconfig_channel(void)
{
#if defined(CONFIG_WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL) && \
    (CONFIG_WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL == 1)
    return BOARD_CH_SCALE_1;
#else
    return BOARD_CH_SCALE_2;
#endif
}

static volatile int s_active_channel = -1; /* -1 = not overridden, use Kconfig */

bool board_pinmap_set_active_channel(board_channel_t channel)
{
    if ((int)channel < 0 || (int)channel >= (int)BOARD_CH_SCALE_COUNT) return false;
    s_active_channel = (int)channel;
    return true;
}

board_channel_t board_pinmap_active_channel(void)
{
    int sel = s_active_channel;
    if (sel == (int)BOARD_CH_SCALE_1 || sel == (int)BOARD_CH_SCALE_2) {
        return (board_channel_t)sel;
    }
    return board_pinmap_kconfig_channel();
}

bool board_pinmap_fill_link(board_channel_t channel, int baud_rate,
                            scale_link_config_t *out)
{
    if (out == NULL) return false;

    const board_scale_link_t *link = board_pinmap_get(channel);
    if (link == NULL) return false;

    out->uart_port = link->uart_port;
    out->rx_gpio = link->rx_gpio;
    out->tx_gpio = link->tx_gpio;
    out->baud_rate = (baud_rate > 0) ? baud_rate : link->baud_rate;
    out->transport = link->transport;
    out->channel = (int)link->channel;
    return true;
}

void board_pinmap_log(board_channel_t channel)
{
    const board_scale_link_t *link = board_pinmap_get(channel);
    if (link == NULL) return;

    const char *xport = (link->transport == SCALE_XPORT_RS485_AUTO_DIR)
                            ? "RS485" : "RS232";

    ESP_LOGI(TAG, "%s CH%u | UART%d | TX=GPIO%d | RX=GPIO%d | DE=%s",
             xport, (unsigned)(link->channel + 1U), link->uart_port,
             link->tx_gpio, link->rx_gpio,
             board_de_re_mode_name(link->de_re_mode));

    if (link->gpio15_strap_pin) {
        ESP_LOGW(TAG, "%s CH%u RX is on GPIO15 (MTDO strapping pad): a device "
                      "driving the bus low during reset can disturb boot. "
                      "If boot is unstable with the receiver connected, verify the "
                      "PCB pull-up (10 kOhm to 3.3 V) instead of remapping pins.",
                 xport, (unsigned)(link->channel + 1U));
    }

    if (link->rx_is_input_only_pad) {
        ESP_LOGW(TAG, "%s CH%u RX GPIO%d is an input-only pad; idle-high must "
                      "come from the line receiver",
                 xport, (unsigned)(link->channel + 1U), link->rx_gpio);
    }
}

void board_pinmap_log_all(void)
{
    ESP_LOGI(TAG, "board variant=%s | compiled-default scale channel=CH%u",
             BOARD_VARIANT_NAME,
             (unsigned)(board_pinmap_active_channel() + 1U));

    for (int i = 0; i < (int)BOARD_CH_SCALE_COUNT; ++i) {
        board_pinmap_log((board_channel_t)i);
    }

    const board_scale_link_t *active =
        board_pinmap_get(board_pinmap_active_channel());
    if (active != NULL) {
        ESP_LOGI(TAG, "connector=%s A=%s B=%s GND=%s (all %s)",
                 active->connector.connector, active->connector.a_pin,
                 active->connector.b_pin, active->connector.gnd_pin,
                 CONNECTOR_UNVERIFIED);
    }
}

void board_pinmap_check_strap_pad(void)
{
    const board_scale_link_t *link = board_pinmap_get(BOARD_CH_SCALE_1);
    if (link == NULL || !link->gpio15_strap_pin) return;

    /*
     * Read the pad as an input without driving it. This is a report-only
     * check: it must never block boot or reconfigure the UART that already
     * owns the pad.
     */
    gpio_num_t pad = (gpio_num_t)BOARD_GPIO15_MTDO_STRAP;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (unsigned)pad,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO15 strap-pad check skipped: %s", esp_err_to_name(err));
        return;
    }

    int level = gpio_get_level(pad);
    ESP_LOGI(TAG, "GPIO15 (MTDO) strap pad level at boot = %d (%s)",
             level, level ? "HIGH, normal boot" : "LOW, boot may be disturbed");
    if (level == 0) {
        ESP_LOGW(TAG, "GPIO15 is LOW after reset. If Channel 1 shows boot "
                      "instability with the RS-485 receiver connected, use "
                      "the pull-up on GPIO15 on the PCB; do not remap pins "
                      "without the schematic.");
    }
}
