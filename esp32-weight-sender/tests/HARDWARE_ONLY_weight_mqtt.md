# Hardware-only checks: weight over MQTT (REQ-WMQ)

Host (QEMU) coverage lives in tests/qemu/main/test_weight_mqtt.c. The items below cannot be proven on the host. Do not run them against a production pump line. Bench broker and bench scale only.

| REQ | Setup | Action | Expected observable | Pass criteria |
|---|---|---|---|---|
| REQ-WMQ-02 | CI-150A or serial replay at maximum line rate, debug build | Run 1 h, read weight_tx uxTaskGetStackHighWaterMark from the debug log | HWM log line each minute | minimum free stack >= 1024 B throughout |
| REQ-WMQ-05 | Frame generator at maximum rate, broker subscribed to telemetry/status | Run 24 h | status carries heap_min_kb, weight_published, weight_overwritten, weight_dropped_stale, rx_dropped_full, hb counters | heap_min_kb falls by <= 8 KB over 24 h; counters present in every status |
| REQ-WMQ-14 | Device connected, mosquitto_sub on cas/+/# | Stop the broker for 60 s, restart it | one reconnect per device in 1-30 s backoff window with jitter, one birth, status dropped_offline > 0 after reconnect | no duplicate birth, no reconnect storm |
| REQ-WMQ-06 | Live scale on CH1, subscribe to telemetry/weight | Unplug the RS-485 cable; wait 5 s; replug | heartbeats every ~1000 ms with advancing uptime_ms and age_ms while live; none carrying weight_g after age_ms > 3000 | no weight_g message older than 3000 ms; resumes on first REAL sample |
| REQ-WMQ-24 | Device with valid Wi-Fi | Write garbage into the mqtt_cfg NVS key (nvs partition tool), power cycle | one logged degraded line with reason code; Wi-Fi up, weight WebSocket serving, provisioning portal reachable | boot completes, no reboot loop |
| REQ-WMQ-23 | Provisioned broker blob | Cut power during the portal save several times (random delay) | after each boot the log shows source nvs (complete old or new record) or kconfig fallback with reason | never a mixed record; mqtt_link_start never uses partial data |
| REQ-WMQ-25 | Portal in AP mode | Open the page, save an invalid URI, then a valid one | page shows source nvs/kconfig and redacted URI; invalid URI rejected before any write; password field never pre-filled | page source has no password |
| REQ-WMQ-26 | Connected device | Change broker config via portal without reboot | running client keeps the old broker until reboot or mqtt_link_stop/start | no mid-session reconfiguration |
| REQ-WMQ-51 | Debug build with the mqtt_pub hang hook | Hang mqtt_pub | log line HB_FROZEN_MQTT_PUB after 10 s; status hb[] counter stops rising for that task only | fault logged once per episode with reason code |
| REQ-WMQ-52 | Same hook | Hang mqtt_pub 30 s; repeat within 10 min | first: mqtt_link_stop/start logged; second: logged esp_restart with reason | restart cap honored, nothing else restarted |
| REQ-WMQ-30 | Bench indicator, encoder still unverified | Send ZERO twice within 2 s from mosquitto_pub | second ack FAILED reason "rate limited" | no frame transmitted for either (encoder NOT_VERIFIED) |
| REQ-WMQ-18 | Same | Send ZERO with ttl_ms 10001 | ack FAILED reason "ttl too long" | no queueing |
