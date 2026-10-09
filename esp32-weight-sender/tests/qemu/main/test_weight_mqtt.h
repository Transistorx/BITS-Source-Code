#ifndef TEST_WEIGHT_MQTT_H
#define TEST_WEIGHT_MQTT_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void test_weight_mqtt_run(QueueHandle_t manager);

#endif
