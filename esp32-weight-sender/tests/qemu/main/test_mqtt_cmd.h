#ifndef TEST_MQTT_CMD_H
#define TEST_MQTT_CMD_H

/* mqtt_link_core (topics, presence, backoff, bounded queue) and scale_cmd
 * (ZERO/TARE validation, dedupe, execution) against mock scale services. */
void test_mqtt_cmd_run(void);

#endif /* TEST_MQTT_CMD_H */
