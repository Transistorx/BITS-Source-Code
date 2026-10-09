#include "remote_cmd.h"

#include <string.h>

#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_link.h"
#include "mqtt_link_core.h"
#include "nvs_config.h"
#include "scale_cmd.h"
#include "weight_source.h"

static const char *TAG = "CMD";

#define SCALE_CMD_TASK_STACK 4096
#define SCALE_CMD_TASK_PRIO  2
#define SCALE_CMD_SETTLE_MS  2000U

static scale_cmd_t s_cmd;

static uint32_t now_ms(void)
{
    return weight_source_now_ms();
}

static bool link_online(const char **state_name)
{
    cas_link_state_t st = weight_source_cas_link_state();
    *state_name = cas_link_state_name(st);
    return st == CAS_LINK_ONLINE;
}

/* Only genuine fresh CAS data: a stale or absent scale yields false, never a
 * cached or zero value. */
static bool post_sample(uint8_t channel, uint32_t *seq, int32_t *weight_g, bool *stable)
{
    weight_sample_t s;
    if (weight_source_get(&s) != WEIGHT_SOURCE_RESULT_REAL) return false;
    if (channel == 1U && s.channel1_valid) {
        *weight_g = s.channel1_weight_g;
        *stable = s.channel1_stable;
    } else if (channel == 2U && s.channel2_valid) {
        *weight_g = s.channel2_weight_g;
        *stable = s.channel2_stable;
    } else {
        return false;
    }
    *seq = s.sequence;
    return true;
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void publish_ack(const scale_cmd_ack_t *ack)
{
    char json[MQTT_LINK_PAYLOAD_MAX];
    if (scale_cmd_build_ack_json(&s_cmd, ack, json, sizeof(json)) == 0U ||
        !mqtt_link_publish(MQTT_SUFFIX_ACK, json, 1, false)) {
        ESP_LOGW(TAG, "ACK not queued id=%lu", (unsigned long)ack->command_id);
        return;
    }
    if (ack->applied) {
        ESP_LOGI(TAG, "id=%lu APPLIED", (unsigned long)ack->command_id);
    } else {
        ESP_LOGW(TAG, "id=%lu FAILED reason=%s", (unsigned long)ack->command_id, ack->reason);
    }
}

/* mqtt_pub task context. */
static void on_command(const char *payload, size_t len, bool retain, uint32_t recv_ms)
{
    scale_cmd_ack_t ack;
    switch (scale_cmd_submit(&s_cmd, payload, len, retain, recv_ms, &ack)) {
    case SCALE_SUBMIT_ACK:
        publish_ack(&ack);
        break;
    case SCALE_SUBMIT_QUEUED:
        ESP_LOGI(TAG, "%s requested id=%lu ch=CH%u",
                 ack.type == SCALE_CMD_TARE ? "TARE" : "ZERO",
                 (unsigned long)ack.command_id, (unsigned)ack.channel);
        break;
    default:
        break;
    }
}

/* The only task allowed to touch the scale transport for commands. */
static void scale_cmd_task(void *arg)
{
    (void)arg;
    scale_cmd_ack_t ack;
    for (;;) {
        if (scale_cmd_run_one(&s_cmd, 1000U, &ack)) publish_ack(&ack);
    }
}

esp_err_t remote_cmd_start(void)
{
    scale_cmd_cfg_t cfg = {
        .now_ms = now_ms,
        .link_online = link_online,
        .post_sample = post_sample,
        .send_frame = NULL, /* no verified frame exists; see scale_cmd_encode.c */
        .delay_ms = delay_ms,
        .encode = NULL,
        .active_channel = (uint8_t)(board_pinmap_active_channel() + 1),
        .settle_timeout_ms = SCALE_CMD_SETTLE_MS,
    };
    (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, cfg.device_id, sizeof(cfg.device_id));
    strlcpy(cfg.boot_id, mqtt_link_boot_id(), sizeof(cfg.boot_id));
    if (!scale_cmd_init(&s_cmd, &cfg)) return ESP_ERR_NO_MEM;
    mqtt_link_set_command_handler(on_command);
    if (xTaskCreate(scale_cmd_task, "scale_cmd", SCALE_CMD_TASK_STACK, NULL,
                    SCALE_CMD_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
