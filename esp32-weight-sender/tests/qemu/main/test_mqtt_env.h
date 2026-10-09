#ifndef TEST_MQTT_ENV_H
#define TEST_MQTT_ENV_H

/* MQTT v1 envelope: boot_id, birth, weight/ctl + telemetry/weight, dedupe, drops. */
void test_mqtt_env_run(void);

#endif /* TEST_MQTT_ENV_H */
