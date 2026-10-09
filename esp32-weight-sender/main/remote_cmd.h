#ifndef REMOTE_CMD_H
#define REMOTE_CMD_H

#include "esp_err.h"

/* Wires cas/{device}/commands to scale_cmd: installs the MQTT command handler,
 * starts the scale_cmd task and publishes each ACK to commands/ack (QoS 1).
 * Call after nvs_config_ensure_device_id() and before mqtt_link_start(). */
esp_err_t remote_cmd_start(void);

#endif /* REMOTE_CMD_H */
