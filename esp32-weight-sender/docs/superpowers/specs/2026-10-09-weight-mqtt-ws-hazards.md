# Hazard and requirements analysis: per-frame weight over MQTT, runtime broker config, server WebSocket live path

Date: 2026-10-09. Class C change (NVS schema change, new remote command path). Analysis before design; nothing here is implemented.
Scope: esp32-weight-sender firmware (ESP-IDF C) and server/ (FastAPI, paho-mqtt). Standards: .claude/team/FW_STANDARDS.md.
The ESP32 is not a certified safety device. Every mitigation below that protects the process must be backed by hardware-level protection independent of this firmware and of the network (see HZ-21).

Legend: Sev = low/med/high. [HOST] = verifiable on the host or QEMU suite without hardware. [HW] = hardware fault injection only. [SEC] = needs fw-security review.
Existing code is cited as file:line; paths are relative to the repo root.

## 0. Baseline facts (cited)

- Scale input is passive receive-only on CH1 = UART1, TX GPIO13, RX GPIO15, 9600 baud (esp32-weight-sender/components/board/include/board_pins.h:111-113,120). Default channel 1 (components/board/Kconfig:41-44), NVS `scale_ch` override (main/app_main.c:713-722). Parsers are out of scope.
- weight_tx task: 5120 B stack, prio 3, polls every 20 ms (main/app_main.c:799, :44) and only offers REAL samples to MQTT (main/app_main.c:251-275). mqtt_link_weight_frame formats into a 320 B stack buffer (components/mqtt_link/mqtt_link.c:142), dedupes by cas_seq (components/mqtt_link_core/mqtt_link_core.c:157-165) and gates telemetry at 500 ms (mqtt_link_core.h:67, mqtt_link_core.c:167-173, mqtt_link.c:154-160).
- One shared FreeRTOS queue of 8 items, about 620 B each, carries BOTH outbound publishes and inbound commands (mqtt_link_core.h:31,150-158; mqtt_link_core.c:199-245; mqtt_link.c:44,135,199-203). Zero-timeout push; a full queue drops and counts (mqtt_link_core.c:203-210).
- mqtt_pub task: prio 2, 6144 B stack, is the only publisher AND the command dispatcher (mqtt_link.c:38-40,264-323). esp-mqtt client task prio 2, outbox limit 8192 B, in/out buffers 1024 B, network timeout 5000 ms, auto-reconnect disabled, reconnect 1-30 s with jitter (mqtt_link.c:425-444; mqtt_link_core.c:44-56). Keepalive 30 s (components/mqtt_link/Kconfig:27-30).
- QoS0 while disconnected is dropped at the door (mqtt_link.c:130-134, :227); QoS1 acks are parked in the esp-mqtt outbox for the next connect (mqtt_link.c:223-226).
- Broker URI, username, password are compile-time Kconfig only (mqtt_link.c:21-32, :391-394, :426, :445-448). Empty URI disables MQTT (mqtt_link.c:391-394). URI is logged redacted (mqtt_link_core.c:175-192, mqtt_link.c:487-489).
- NVS: keys come from a single table; key strings limited to 15 chars; no schema version, CRC or atomic multi-key write; each set commits separately (components/nvs_config/include/nvs_config.h:20,26-37; components/nvs_config/nvs_config.c:111-126). A corrupt partition is erased wholesale, including Wi-Fi credentials (nvs_config.c:49-59). Provisioning writes SSID then password as two commits over plain HTTP on the AP (components/provisioning/provisioning.c:470-505).
- Commands: retained ignored (mqtt_link_core.c:222-225; components/scale_cmd/scale_cmd.c:221-222); command_id required, uint32 nonzero (scale_cmd.c:226-229); dedupe by id over 16 slots, queue depth 4, TTL max 60000 ms (components/scale_cmd/include/scale_cmd.h:51-53; scale_cmd.c:234-245); device_id and channel checked (scale_cmd.c:267-274); ttl_ms required and measured from device receive time recv_ms (scale_cmd.c:277-284, :339-342); one running at a time (scale_cmd.c:285); scale link must be ONLINE (scale_cmd.c:288-294, main/remote_cmd.c:28-33). Time base is esp_timer uptime only (components/weight_source/weight_source.c:173-180); there is no wall clock on the device.
- ACK carries command_id, state, result, error/reason, device_id, boot_id, channel_id and optional weight_g/stable (scale_cmd.c:435-464); published QoS1 non-retained (main/remote_cmd.c:59-66). Correlation is by command_id only; there is no originating-client field.
- Today no ZERO/TARE can reach the scale: send_frame is NULL (remote_cmd.c:108) and the encoder is unverified (scale_cmd.h:38-39, scale_cmd.c:354-361), so every accepted command ends FAILED/UNVERIFIED or "no scale transmit path". Settle wait is 2000 ms (remote_cmd.c:19).
- Presence: LWT and birth retained QoS1 on cas/{dev}/status; birth carries boot_id (mqtt_link.c:425-435, :238-241; mqtt_link_core.c:100-112).
- Scale freshness: reader offline timeout 3000 ms (components/scale_hal/scale_reader.c:46,825); weight_source STALE threshold 3000 ms (components/weight_source/include/weight_source.h:28-31); reader loop gap and stall diagnostics exist (scale_reader.c:33-34, :833-838).
- Server: subscriptions include cas/+/telemetry/weight QoS0 and never weight/ctl (server/app/mqtt_bridge.py:46-51). "live" and "weight" kinds are handled INLINE on the paho thread (mqtt_bridge.py:702-714); SQL kinds go through a 1000-item queue (mqtt_bridge.py:57,699-719). MqttWeight requires fields uptime_ms, channel (CH1|CH2), weight_g, stable, age_ms (mqtt_bridge.py:332-339). Firmware emits src_uart:"UARTn", scale_id, cas_seq, seq, boot_id and NO "channel" field (mqtt_link_core.c:139-145), so today every telemetry/weight message is rejected by the server model (ValidationError path mqtt_bridge.py:398-400).
- live_state is process-local and documented as one-worker only (server/app/services/live_state.py:1-6); README runs uvicorn with --workers 1 (server/README.md:54). Sender weight validity is age_ms (reported + time since receipt) <= 3000 (live_state.py:164-176). Ordering rejects a duplicate uptime_ms (delta == 0) and regressions unless 10 s have elapsed (live_state.py:91-113).
- paho client_id is the fixed string "dispense-server" (mqtt_bridge.py:813). Command publish is QoS1 non-retained and only on a live link (mqtt_bridge.py:161-176, :562-579). Every DeviceCommand row insert is auto-published by an ORM hook (mqtt_bridge.py:250-274); ACKs are matched to a DeviceCommand row by command_id and otherwise ignored (mqtt_bridge.py:495-508); PENDING rows are republished once at startup recovery (mqtt_bridge.py:752-785); a boot_id change closes unacked rows (mqtt_bridge.py:423-445).
- API key: empty API_KEY means writes are open (server/app/auth.py:15-19; server/README.md:299). SSE stream is 10 Hz with a 1 s forced resend (server/app/routes/live.py:25-36). Current live.js is read-only and SSE-based (server/app/static/js/live.js:1-4).

## 1. Hazard table

### HZ-01 Per-frame publish burst exhausts queue, CPU, heap or stack  (Sev: med)
- Failure mode: removing the 500 ms gate makes publish rate equal the indicator frame rate. Upper bound from the line rate: 9600 baud, 20 B body plus terminator (scale_reader.c:37) gives tens of frames per second; weight_tx offers at most 50/s (20 ms poll). Each publish is up to 256 B JSON (mqtt_link_core.h:32) plus esp-mqtt packet allocation.
- Cause: indicator streams faster than the mqtt_pub task and Wi-Fi can drain; esp_mqtt_client_publish on QoS0 writes synchronously on the pub task and can block up to network.timeout_ms = 5000 (mqtt_link.c:438) on a stalled socket.
- Effect on process/VFD: none directly (weight WS path to the relay controller is separate, mqtt_link.h:4-7). Effect on firmware: the 8-slot queue saturates; because it is shared, inbound ZERO/TARE commands are dropped (dropped_full) behind weight items; Wi-Fi task CPU rises and can widen the reader loop gap (scale_reader.c:833-838); heap churn and fragmentation over long uptime; 320 B JSON on the weight_tx stack with 5120 B total.
- Detection: mqtt_link_stats dropped_full/queue_depth/publish_failed (mqtt_link.c:88-98) in the health line; reader loop_gap_max_ms and uart_event_queue_saturations (scale_reader.c:837,841); heap low-water mark.
- Safe state: drop telemetry, never block the reader or weight_tx; commands keep a reserved path.
- Latency budget: weight_tx loop must stay <= 20 ms per iteration; reader loop gap must stay below CONFIG_SCALE_READER_STALL_GAP_MS (250 ms, scale_reader.c:33-34).
- Requirements: REQ-WMQ-01, 02, 03, 04, 05.
- Hardware: none.

### HZ-02 Shared queue and single pub task starve commands and acks  (Sev: med)
- Failure mode: weight items fill the queue so command RX items are refused (mqtt_link_core.c:205-207); or a blocking publish delays command dispatch (mqtt_link.c:312-319) past ttl_ms (3000 ms from server), so ZERO/TARE are rejected "expired before start" (scale_cmd.c:281-284) or acks are late.
- Cause: one queue, one task, per-frame rate (see HZ-01).
- Effect: operator sees timeouts, retries, possible duplicate intent (see HZ-15). Process: none directly.
- Detection: dropped_full counter rising while commands fail; server 3 s ack timeout rate.
- Safe state: commands dropped are never executed late; acks still correlate.
- Latency: command from broker to scale_cmd queue <= 200 ms with broker up.
- Requirements: REQ-WMQ-03, 04, 20.

### HZ-03 Heartbeat republish shows a stale or dead scale as live  (Sev: high)
- Failure mode: the 1 s heartbeat resends the last value after the indicator stopped (cable out, indicator off, parser desync). A consumer that ignores age_ms displays a frozen number as live; the future pump controller could dose on it (HZ-21).
- Cause: heartbeat emitted from cached sample rather than from weight_source; age_ms not advanced; server or UI treats "message received" as "fresh".
- Effect: wrong weight presented as current; process dosing error if any consumer acts on it.
- Detection: age_ms in payload; weight_source STALE/OFFLINE transition (weight_source.c:303); server validity rule age <= 3000 (live_state.py:170).
- Safe state: no weight value, explicit invalid flag; UI shows "no data".
- Latency: a stopped indicator must be shown invalid within 3000 ms plus one heartbeat (<= 4 s end to end).
- Requirements: REQ-WMQ-06, 07, 08, 09.
- Hardware: none for display; for control see HZ-21.

### HZ-04 Replayed, duplicated or spoofed weight accepted as live  (Sev: med)
- Failure mode: a QoS0 message replayed by a broker bridge, a retained copy, a second device publishing on the same device id, or a heartbeat with unchanged uptime_ms.
- Cause: topic device id is the only identity; no signature; retained flag only partly honoured.
- Effect: server shows another source's value; ordering guard rejects a heartbeat carrying the same uptime_ms (live_state.py:107-109) so the UI goes stale although the device is alive.
- Detection: live_state rejected counter (live_state.py:133,156); boot_id/seq regression.
- Safe state: reject and count; never overwrite a newer record.
- Requirements: REQ-WMQ-08, 10, 11. [SEC]
- Hardware: broker ACL per device id (outside firmware).

### HZ-05 Firmware and server weight schemas disagree; stream silently rejected  (Sev: med)
- Failure mode: today the firmware body has src_uart and no channel field (mqtt_link_core.c:139-145) while MqttWeight requires channel (mqtt_bridge.py:336); every message is rejected and only a rate-limited log line reports it (mqtt_bridge.py:398-400). The UI shows "no data" while the device is healthy.
- Cause: contract drift between CONTRACT section 8 producers and the server model; no end-to-end test.
- Effect: observability loss; operator may suspect the scale; no process effect.
- Detection: live_state rejected counter; absence of senders in snapshot.
- Safe state: reject (fail closed) is correct; the failure must be visible.
- Requirements: REQ-WMQ-12, 13.

### HZ-06 Broker loss and reconnect bursts  (Sev: med)
- Failure mode: broker restart, Wi-Fi roam, or AP loss. QoS0 weight is dropped (correct); on reconnect every device resubscribes and republishes birth (mqtt_link.c:230-242); the server drops parked publishes (mqtt_bridge.py:615-634).
- Cause: network; many devices reconnecting at once with 1-30 s backoff and 25% jitter (mqtt_link_core.c:51-56).
- Effect: gaps in live data; server WS clients see stale data; a command issued during the gap is never published (publish_command returns False, mqtt_bridge.py:167-169) and must be reported as not sent.
- Detection: device dropped_offline counter; LWT online:false (mqtt_link.c:429-435); server mqtt_state DISCONNECTED (mqtt_bridge.py:686-694).
- Safe state: UI marks all senders invalid after 3 s; WS command path refuses new commands while bridge is not CONNECTED.
- Latency: broker loss detected on the device within keepalive x 1.5 = 45 s worst case (HZ-07); server-side within paho keepalive 30 s (mqtt_bridge.py:834).
- Requirements: REQ-WMQ-14, 15, 16.

### HZ-07 Half-open TCP: weight frozen, late command delivery with fresh TTL  (Sev: high)
- Failure mode: the socket stalls without a FIN (AP power cycle, NAT drop). The device keeps thinking it is connected until keepalive fails (<= 45 s). Packets buffered on the path, including a ZERO/TARE published during the stall, are delivered when the socket resumes. The device stamps recv_ms at delivery (mqtt_link.c:199-203) and the TTL check starts then (scale_cmd.c:281), so a command that is minutes old on the server clock looks fresh on the device.
- Cause: no wall clock on the device; issued_at is not in the parsed field set (scale_cmd.c:13) and could not be compared anyway.
- Effect: ZERO or TARE applied long after the operator gave up; a second manual ZERO may follow (double action); if the hopper was loaded in between, the tare is wrong and the future pump target shifts (HZ-21).
- Detection: server ack timeout; device ack arrives after server marked the command unknown.
- Safe state: server treats an unacked command as outcome unknown and tells the operator; device must not act on commands delivered after the link was known stalled.
- Latency: residual window equals keepalive detection time; must be bounded and documented.
- Requirements: REQ-WMQ-17, 18, 19, 20. [SEC]
- Hardware: none; residual risk accepted only because ZERO/TARE do not move an actuator. Re-evaluate when the pump exists.

### HZ-08 NVS broker configuration missing, corrupt, partial or hostile  (Sev: high)
- Failure mode: (a) keys absent after OTA: must fall back to Kconfig; (b) power loss between the three separate commits for URI/user/pass (nvs_config.c:116-121) leaves mismatched credentials and a connect-refused loop (mqtt_link.c:254-256); (c) malformed URI makes esp_mqtt_client_init fail and MQTT stays off (mqtt_link.c:455-459) with no recovery path except reprovisioning; (d) an attacker on the provisioning AP points the device at a hostile broker that then issues ZERO/TARE; (e) nvs_flash_init corruption erases the whole partition including Wi-Fi credentials (nvs_config.c:49-59), forcing the provisioning path; (f) a URI with a long host or userinfo overflows the 96 B redaction buffer (mqtt_link.c:487) or the esp-mqtt limits.
- Cause: NVS schema has no version, CRC or transactional write; portal is unauthenticated HTTP.
- Effect: MQTT permanently down (observability loss only; weight WS path unaffected) or device joined to the wrong broker (command injection).
- Detection: boot log of config source (kconfig/nvs) with redacted URI; connect-refused counter; birth never seen by the intended broker.
- Safe state: fall back to Kconfig values; never boot-loop; never lose Wi-Fi credentials because of a bad broker key; MQTT off is a degraded state with a reason code.
- Latency: fallback decided at mqtt_link_start, before any connection attempt.
- Requirements: REQ-WMQ-21, 22, 23, 24, 25, 26. [SEC]
- Hardware: OPEN Q3 (flash encryption / secure boot status).

### HZ-09 Credential exposure  (Sev: med)
- Failure mode: broker password visible in logs, in the provisioning page, in git (sdkconfig default URI), or readable from flash.
- Cause: plaintext NVS; Kconfig defaults committed; portal over plain HTTP; new log lines echoing config.
- Effect: broker takeover, command injection on all devices sharing the credential.
- Detection: code inspection; log review.
- Safe state: credentials never logged; portal never pre-fills a stored password.
- Requirements: REQ-WMQ-27, 28. [SEC]
- Hardware: flash encryption is a production-hardening checklist item (FW_STANDARDS section 4), never applied without the user's command.

### HZ-10 Unauthorized ZERO/TARE via MQTT  (Sev: med; high once an actuator depends on tare)
- Failure mode: any client with publish rights on cas/{dev}/commands can zero or tare the scale. The device only checks device_id and channel (scale_cmd.c:267-274); there is no signature or sender authentication.
- Cause: broker without ACLs or with shared credentials; anonymous broker (Kconfig default has no username, mqtt_link/Kconfig:17-23).
- Effect: weight baseline shifted while a batch is running; wrong dose later (HZ-21).
- Detection: ack published for every executed command; server sees an ack for a command_id it never issued.
- Safe state: reject; log; require scale ONLINE (already) and, when a batch controller exists, refuse ZERO/TARE while dosing.
- Requirements: REQ-WMQ-29, 30, 31. [SEC]
- Hardware/infra: broker ACL so each device may publish only cas/{dev}/# and subscribe only cas/{dev}/commands; the server identity is the only publisher to cas/+/commands. TLS on the broker.

### HZ-11 Unauthorized, flooded or duplicated ZERO/TARE via WebSocket  (Sev: med)
- Failure mode: WS inbound {type:cmd} accepted without key (API_KEY empty = open, auth.py:15-19); a client loops commands; two browser tabs send the same intent; device id in the message not in the whitelist.
- Cause: new inbound path on a previously read-only live channel (live.js:3).
- Effect: command storm fills the device scale_cmd queue (depth 4) and dedupe slots (16); constant tare changes.
- Detection: server-side per-client and per-device rate counters; device "command queue full" rejections (scale_cmd.c:295).
- Safe state: refuse; close the socket on repeated abuse.
- Requirements: REQ-WMQ-32, 33, 34, 35. [SEC]

### HZ-12 Command id collision and ack misrouting between WS path and DB path  (Sev: med)
- Failure mode: the device dedupes by uint32 command_id (scale_cmd.c:234-245) and re-ACKs the stored result for a repeated id. If WS commands use an id space separate from DeviceCommand.id, a collision makes the device answer a new command with an old ack and skip execution, while the server attributes the ack to the wrong client. If WS commands ARE DeviceCommand rows, the ORM hook also publishes them (mqtt_bridge.py:250-274) and startup recovery republishes them later (mqtt_bridge.py:752-785); acks for ids without a row are ignored (mqtt_bridge.py:499-501).
- Cause: two publishers of the same topic with different bookkeeping.
- Effect: silent non-execution, wrong ack shown, replay at server restart.
- Detection: ack command_id not in the in-flight map; duplicate publish counters.
- Safe state: exactly one id authority; WS commands never republished.
- Requirements: REQ-WMQ-36, 37, 38.

### HZ-13 Retained command replay  (Sev: low, mitigated)
- Failure mode: a retained message on cas/{dev}/commands is delivered on every subscribe; the device ignores retained (mqtt_link_core.c:222-225, scale_cmd.c:221-222) and the server never sets retain (mqtt_bridge.py:171-173). The WS path must keep both properties, and a broker ACL must stop third parties from setting a retained command.
- Requirements: REQ-WMQ-39, 40.

### HZ-14 Server ack timeout shorter than the device's execution time; operator double-issues  (Sev: med)
- Failure mode: server waits 3 s. Device path is queue -> execute -> up to 2000 ms settle (remote_cmd.c:19) -> QoS1 ack over Wi-Fi, so an APPLIED ack can legitimately arrive after 3 s. UI shows "timeout"; operator sends again; two tares run.
- Cause: ttl_ms 3000 and ack timeout 3000 chosen without summing device budget.
- Effect: double tare, confusing state; HZ-21 amplifies.
- Detection: late ack after timeout (device sends it regardless).
- Safe state: the channel stays locked until ack or (ttl + settle + margin); a late ack is surfaced as "late APPLIED", never dropped silently.
- Requirements: REQ-WMQ-19, 20, 41.

### HZ-15 Device reboot mid-command  (Sev: low)
- Failure mode: the device restarts between accepting a command and acking it. Dedupe slots are RAM only; boot_id changes in the next birth. Server closes DB rows on boot_id change (mqtt_bridge.py:423-445) but a WS in-flight entry is not a DB row.
- Effect: WS client waits the full timeout; a resend after reboot is executed (new boot, empty dedupe) which is acceptable because the operator re-issued.
- Requirements: REQ-WMQ-42.

### HZ-16 paho thread to asyncio handoff fails or blocks  (Sev: med)
- Failure mode: loop.call_soon_threadsafe is called with a loop reference captured before the loop exists, after shutdown, or in a different worker; RuntimeError on the paho thread can kill the network loop thread so the bridge looks CONNECTED but delivers nothing. Any blocking work in the inline "weight" path (mqtt_bridge.py:702-714) blocks all MQTT I/O including command publish and keepalive.
- Cause: lifecycle ordering (mqtt_bridge.start() runs in lifespan before/independent of the loop reference, server/app/main.py:56-71); exceptions not caught at the callback boundary.
- Effect: live data stops; commands cannot be published; health still says CONNECTED.
- Detection: thread liveness check; watchdog counter of fan-out failures; bridge health must include the last message age.
- Safe state: fan-out failure is counted and dropped, never raised into paho; bridge reports DEGRADED.
- Requirements: REQ-WMQ-43, 44, 45.

### HZ-17 Slow or stalled WebSocket clients  (Sev: low)
- Failure mode: a client on a bad link stops reading; per-client send coroutine backs up; memory grows; one client delays the broadcast loop for all.
- Cause: unbounded send queue or awaiting send under a shared lock.
- Effect: UI lag for everyone; server memory growth.
- Safe state: per-client latest-value slot (drop stale, keep newest), bounded send timeout, disconnect after N consecutive timeouts.
- Requirements: REQ-WMQ-46, 47, 48.

### HZ-18 Multiple server workers  (Sev: med)
- Failure mode: uvicorn --workers > 1 or a second instance. live_state is per process (live_state.py:5); every worker starts its own paho client with the same client_id "dispense-server" (mqtt_bridge.py:813), so the broker evicts the previous session on each connect, producing a reconnect storm; the per-channel in-flight lock is per process, so two workers accept two commands; the ack lands in whichever worker's subscription survived.
- Effect: lost commands, perpetual timeouts, inconsistent live views across clients.
- Detection: repeated connect/disconnect logs; ack-for-unknown-command counter.
- Safe state: refuse to start the bridge in more than one process, or make the client_id unique and move in-flight state to a shared store.
- Requirements: REQ-WMQ-49, 50.

### HZ-19 Task hang or watchdog trip in weight_tx, mqtt_pub or scale_cmd  (Sev: med)
- Failure mode: mqtt_pub blocks inside esp-mqtt (5 s socket timeout) or deadlocks; scale_cmd stalls in settle; weight_tx stalls. The reader keeps running (higher priority), the WS weight path keeps running, but MQTT silently stops and reconnect logic (which lives in the same task, mqtt_link.c:278-309) stops with it.
- Detection: application heartbeat per critical task in the health line; queue_depth stuck at 8 with published not advancing; server sees sender age growing.
- Safe state: server marks invalid at 3 s; device restarts the link or itself after a bounded stall.
- Requirements: REQ-WMQ-51, 52.
- OPEN Q4: task watchdog configuration not found in the tree.

### HZ-20 Scale command transmit on a passive RS-485 bus  (Sev: high once enabled; today inert)
- Failure mode: when a verified encoder and send_frame exist, the ESP32 drives TX on the same pair the indicator is streaming on. Bus contention, wrong DE/RE direction control, or a wrong byte sequence can corrupt the indicator stream or put the indicator into a mode that stops streaming.
- Cause: unverified protocol bytes (scale_cmd.h:11-13); transceiver direction control not analysed.
- Effect: weight stream lost (reader goes STALE/OFFLINE within 3 s), or indicator reconfigured.
- Detection: reader frame error counters; STALE transition right after a command.
- Safe state: keep SCALE_CMD_ENCODER_VERIFIED = 0 and send_frame = NULL until verified on the bench; commands ack FAILED/UNVERIFIED.
- Requirements: REQ-WMQ-53.
- Hardware: OPEN Q1, Q2.

### HZ-21 Design risk record: a future pump acts on this weight through Wi-Fi/MQTT/server only  (Sev: high, out of scope, must be recorded)
- Failure mode: the pump ESP32 doses to a target using weight that arrives via indicator -> this sender -> Wi-Fi -> broker -> server -> network -> pump. Any stall (HZ-03, HZ-06, HZ-07, HZ-16, HZ-18, HZ-19) or a wrong tare (HZ-07, HZ-10, HZ-11, HZ-14) presents a frozen or shifted weight while the pump keeps running: overfill, spill, overpressure or dry run depending on the plant (plant consequences are not known here; OPEN Q6).
- Firmware alone is not sufficient. Required outside this change: an independent, non-network stop: an indicator setpoint relay or hardwired level/limit switch wired into the pump contactor or VFD enable circuit, and/or the pump controller failing safe (stop) when weight age exceeds its own limit, measured locally. The pump must never be started by a network command alone without a local interlock.
- Requirements: REQ-WMQ-54, 55 (recorded as constraints for the future pump change; not implemented here).

## 2. Requirements

Each requirement is a testable "shall". Verification: UT = unit/QEMU test, FI = fault injection, INS = inspection/review, SRV = server pytest. [HOST]/[HW] as defined above.

Firmware: publish path
- REQ-WMQ-01 mqtt_link_weight_frame shall enqueue with zero timeout and return within 1 ms on a full queue; it shall never call esp_mqtt_client_publish directly from weight_tx. UT [HOST]: call 1000 times with the pub task halted, assert all return immediately and dropped_full == 992.
- REQ-WMQ-02 The weight_tx task stack shall be sized with the per-frame JSON buffer in it and the high-water mark logged in debug builds; minimum free shall stay >= 1024 B after 1 h at the maximum frame rate. FI [HW]: run with an indicator or a serial replay at line-rate maximum, read uxTaskGetStackHighWaterMark.
- REQ-WMQ-03 Inbound command items shall never be refused because outbound telemetry filled the queue: either a separate inbound queue or a reserved slot count for RX items. UT [HOST]: fill with 8 publishes, push one RX, assert MQTT_ENQ_OK.
- REQ-WMQ-04 The pub task shall drop telemetry (QoS0) that is older than one heartbeat period when the queue is above a high-water mark, and shall count it. UT [HOST].
- REQ-WMQ-05 Free heap low-water and the mqtt_link stats (published, dropped_full, dropped_offline, queue_depth) shall be published in telemetry/status and shall not degrade more than 8 KB over 24 h at maximum frame rate. FI [HW]: 24 h soak with a frame generator, log minimum free heap.

Firmware: freshness
- REQ-WMQ-06 The 1 s heartbeat shall republish only while weight_source reports REAL (age <= WEIGHT_CAS_STALE_MS); when STALE or OFFLINE it shall publish either nothing or an explicit {"valid":false} envelope without weight_g; it shall never carry a weight value older than 3000 ms. UT [HOST]: stub weight_source, advance time past 3000 ms, assert no weight_g emitted.
- REQ-WMQ-07 Every weight envelope shall carry age_ms computed at publish time (now - sample stamp) and a current uptime_ms, so two heartbeats never have equal uptime_ms. UT [HOST]: two consecutive heartbeats differ in uptime_ms and age_ms grows by the period.
- REQ-WMQ-08 Server live_state shall accept a heartbeat whose weight and cas_seq are unchanged but whose uptime_ms advanced, and shall report valid=false when reported age plus time since receipt > 3000 ms. SRV [HOST]: feed identical weight with advancing uptime, assert accepted; freeze and assert invalid after 3.0 s.
- REQ-WMQ-09 The WS payload shall include weight_valid and age_ms computed by the same rule as the snapshot (live_state._sender_view) and the UI shall render a value only when weight_valid is true. SRV [HOST] plus INS of live.js.

Server and firmware: identity and replay
- REQ-WMQ-10 Server shall reject weight envelopes whose boot_id regresses to a previously seen value for that device within 10 s, and shall count the rejection. SRV [HOST].
- REQ-WMQ-11 Server shall ignore retained messages on telemetry/weight (existing rule mqtt_bridge.py:376-377) and the firmware shall publish weight with retain=false (existing, mqtt_link.c:150,157). INS plus SRV [HOST].
- REQ-WMQ-12 The firmware telemetry/weight envelope and the server MqttWeight model shall be reconciled in one contract revision: the server shall accept channel derived from src_uart ("UART1" -> "CH1") or the firmware shall add a channel field; the chosen form shall be covered by a golden-payload test shared by both suites. UT [HOST] (firmware JSON builder) and SRV [HOST] (server parses the exact firmware string).
- REQ-WMQ-13 The server shall expose the weight reject counter and the last reject reason per device in /api/v1/live/telemetry so a schema mismatch is visible without logs. SRV [HOST].

Server and firmware: broker link
- REQ-WMQ-14 On broker loss the device shall drop QoS0 weight (existing) and shall publish a status counter dropped_offline on reconnect; reconnect backoff shall remain 1-30 s with jitter. UT [HOST] on mqtt_link_core backoff; FI [HW]: kill broker 60 s, verify a single reconnect per device within 1-30 s and no duplicate birth.
- REQ-WMQ-15 The WS command path shall refuse (with an error message to the client) any command while mqtt_state() is not CONNECTED. SRV [HOST].
- REQ-WMQ-16 On bridge disconnect, every in-flight WS command shall be failed with reason "link lost" within 1 s and its channel lock released. SRV [HOST].

Commands: TTL and clock
- REQ-WMQ-17 Device TTL semantics shall be documented as "measured from device receipt" in the contract and the WS/API documentation; the server shall not claim wall-clock expiry on the device. INS.
- REQ-WMQ-18 The device shall reject any command whose ttl_ms exceeds a new compile-time cap for ZERO/TARE (proposed 10000 ms; SCALE_CMD_TTL_MAX_MS is 60000 today) and shall report "ttl too long". UT [HOST].
- REQ-WMQ-19 The server shall keep the per-channel in-flight lock until an ack arrives or (ttl_ms + device settle 2000 ms + 1000 ms margin) elapses, whichever first; the 3 s client-facing timeout shall produce "outcome unknown", not "failed". SRV [HOST].
- REQ-WMQ-20 An ack arriving after the client-facing timeout but before lock release shall be forwarded to the originating client as state APPLIED/FAILED with late=true; after lock release it shall be logged and dropped. SRV [HOST].

NVS broker configuration
- REQ-WMQ-21 Broker URI, username and password shall be read from NVS keys (<= 15 char key names) at mqtt_link_start; an absent key shall fall back to the Kconfig value; an empty NVS URI shall mean "use Kconfig", not "disable". UT [HOST] with an NVS stub: absent, empty, present.
- REQ-WMQ-22 The URI shall be validated before use: scheme mqtt or mqtts, host 1..64 chars from [A-Za-z0-9.-], optional port 1..65535, no userinfo, total length < 96; invalid values shall be logged (redacted) and the Kconfig fallback used. UT [HOST] with a table of good and bad URIs including a 200 char host.
- REQ-WMQ-23 The three values shall be stored under one NVS schema version key; the writer shall write URI, user, pass, then version last; the reader shall treat a version mismatch or a missing version as "incomplete" and fall back to Kconfig for all three. FI [HW]: cut power between commits (or UT with a stub that fails the Nth commit) and assert fallback.
- REQ-WMQ-24 A broker configuration error shall never prevent boot, Wi-Fi, the weight WebSocket path or provisioning; mqtt_link_start failure shall remain a logged degraded state with reason code (existing behaviour mqtt_link.c:116-118 in app_main). FI [HW]: write garbage to the URI key, power cycle, confirm weight path and portal still work.
- REQ-WMQ-25 The provisioning form shall validate the URI with the same function as REQ-WMQ-22 before writing and shall show the active source (nvs/kconfig) without the password. UT [HOST] on the validator; INS on the page.
- REQ-WMQ-26 Changing broker configuration shall require a reboot or an explicit mqtt_link_stop/start; the running client shall never read config mid-session. INS.
- REQ-WMQ-27 Username and password shall never appear in any log line, HTTP response or MQTT payload; the URI shall be logged only through mqtt_link_uri_redact. UT [HOST]: grep the QEMU log for the test password. [SEC]
- REQ-WMQ-28 The committed Kconfig default URI shall not contain credentials and the production checklist shall list flash encryption as the storage protection for NVS secrets (not applied by this change). INS. [SEC]

Command authentication and rate limiting
- REQ-WMQ-29 The device shall accept commands only from the subscribed topic cas/{dev}/commands (existing, mqtt_link.c:314) and shall continue to require device_id match when present; the deployment shall document a broker ACL that lets only the server identity publish to cas/+/commands. INS plus FI [HW]: publish from an unauthorized client id, confirm the broker refuses. [SEC]
- REQ-WMQ-30 The device shall rate-limit accepted ZERO/TARE to at most one every 2 s per channel (reject others with "rate limited"), independent of dedupe. UT [HOST].
- REQ-WMQ-31 Every executed or rejected command shall produce exactly one ack with command_id, boot_id and reason (existing scale_cmd.c:435-464) so a command the server did not issue is detectable; the server shall count acks for unknown ids. SRV [HOST].
- REQ-WMQ-32 WS inbound commands shall require the same X-API-Key check as HTTP writes, supplied at connect (header or first message), and shall be refused when API_KEY is empty unless an explicit BENCH_OPEN_WRITES setting is true. SRV [HOST]. [SEC]
- REQ-WMQ-33 WS commands shall be accepted only for device ids in a configured whitelist and only for cmd in {ZERO, TARE} and channel in {CH1, CH2}; anything else is rejected without publishing. SRV [HOST].
- REQ-WMQ-34 Per client: at most 1 command per second and 10 per minute; per device channel: one in flight. Excess is rejected with a reason; 20 rejections in a minute close the socket. SRV [HOST].
- REQ-WMQ-35 WS command payloads shall be size-limited (<= 512 B) and parsed with a strict model (extra fields forbidden). SRV [HOST].

Command identity and routing
- REQ-WMQ-36 There shall be exactly one command_id authority on the server for cas/{dev}/commands. Either WS commands are DeviceCommand rows (then the WS path shall not publish directly and shall route acks from the existing _ack path) or WS commands use a reserved id range (>= 2^31) that DeviceCommand.id can never reach, enforced by a check constraint. SRV [HOST]: both publishers in one test, assert no id overlap and no double publish.
- REQ-WMQ-37 WS commands shall never be republished by startup_recovery or by the ORM after_commit hook. SRV [HOST]: restart the bridge with an in-flight WS command, assert zero publishes.
- REQ-WMQ-38 Ack routing shall map command_id to the originating WS client; an ack whose id is not in the in-flight map is counted and dropped, never broadcast. SRV [HOST].
- REQ-WMQ-39 WS command publishes shall be QoS1, retain=false, ttl_ms 3000, and shall include device_id, channel_id, type and command_type (the fields scale_cmd parses, scale_cmd.c:13). SRV [HOST]: assert the exact payload.
- REQ-WMQ-40 The device shall keep ignoring retained commands and fragments (existing); a regression test shall cover retain=1 with a valid body. UT [HOST].
- REQ-WMQ-41 The UI shall disable the ZERO/TARE buttons for a channel while a command is in flight and shall show "outcome unknown" on timeout with no automatic retry. INS plus SRV [HOST] for the state the server sends.
- REQ-WMQ-42 On a birth with a new boot_id for a device, the server shall fail that device's in-flight WS commands with reason "device rebooted" and release the locks. SRV [HOST].

Server threading and clients
- REQ-WMQ-43 The paho-to-asyncio bridge shall capture the running loop in the FastAPI lifespan and shall guard call_soon_threadsafe with try/except RuntimeError; failures increment a counter and never propagate into paho callbacks. SRV [HOST]: stop the loop, deliver a message, assert the paho thread survives and the counter increments.
- REQ-WMQ-44 Work on the paho thread for the weight kind shall be bounded to validation plus one dict update plus one call_soon_threadsafe; no awaits, no locks held across I/O, no JSON encoding of the fan-out payload on that thread. INS plus SRV timing test (< 1 ms median).
- REQ-WMQ-45 Bridge health shall include seconds since the last inbound message and the fan-out failure counter; CONNECTED with no inbound for 30 s shall be shown as DEGRADED. SRV [HOST].
- REQ-WMQ-46 Each WS client shall have one latest-value slot per device; a newer value replaces an unsent older one; the number of queued messages per client never exceeds the number of whitelisted devices plus 1 (for acks). SRV [HOST]: pause a client, push 1000 updates, assert one send.
- REQ-WMQ-47 A send that does not complete within 2 s shall disconnect that client only; other clients shall receive the same update within 100 ms. SRV [HOST] with a stalled fake socket.
- REQ-WMQ-48 On connect the server shall send a snapshot identical to /api/v1/live/telemetry before any delta. SRV [HOST].
- REQ-WMQ-49 The server shall refuse to start the MQTT bridge when it detects more than one worker (uvicorn --workers > 1 or WEB_CONCURRENCY > 1) unless MQTT_CLIENT_ID is explicitly set per worker; the README shall state the single-worker constraint for the WS command path. SRV [HOST] via env; INS.
- REQ-WMQ-50 The paho client_id shall be configurable (MQTT_CLIENT_ID, already used by server/app/service.py:343) and default to a per-process unique suffix so two processes never evict each other. SRV [HOST].

Task supervision
- REQ-WMQ-51 The weight_tx, mqtt_pub and scale_cmd tasks shall each bump an application heartbeat counter; the status publisher shall report all three, and a counter frozen for 10 s shall be logged as a fault with a reason code. UT [HOST] on the checker logic; FI [HW]: hang mqtt_pub with a debug hook and observe the fault.
- REQ-WMQ-52 A frozen mqtt_pub heartbeat for 30 s shall trigger mqtt_link_stop/start; a second occurrence within 10 min shall trigger a logged esp_restart (non-safety auto-recovery with cap, per FW_STANDARDS section 1). FI [HW].

Scale transmit path
- REQ-WMQ-53 SCALE_CMD_ENCODER_VERIFIED shall remain 0 and send_frame NULL until the CI-150A command frames and the RS-485 direction control are verified on hardware and recorded; the bench procedure shall show the weight stream continuing uncorrupted during and after a transmitted frame. FI [HW]; INS of the recorded evidence. See OPEN Q1, Q2.

Pump risk constraints (recorded for the future pump change; not implemented here)
- REQ-WMQ-54 Any controller that starts or keeps a pump running based on this weight shall stop the pump locally when its own measured weight age exceeds a local limit (<= 3000 ms proposed) or when weight_valid is false, without any network round trip. FI [HW] on the pump controller.
- REQ-WMQ-55 The pump drive (contactor or VFD enable) shall have a hardwired stop independent of all ESP32s and of the network: an indicator setpoint relay or a limit switch wired into the enable circuit; its presence shall be verified before any network-driven dosing is enabled. INS plus FI [HW]. Hardware details are unknown (OPEN Q6).

## 3. Traceability summary

| Hazard | Requirements | Host-testable | Hardware-only |
|---|---|---|---|
| HZ-01 | 01,02,03,04,05 | 01,03,04 | 02,05 |
| HZ-02 | 03,04,20 | 03,04,20 | - |
| HZ-03 | 06,07,08,09 | all | - |
| HZ-04 | 08,10,11 | all | - |
| HZ-05 | 12,13 | all | - |
| HZ-06 | 14,15,16 | 14(core),15,16 | 14(broker kill) |
| HZ-07 | 17,18,19,20 | 18,19,20 | - |
| HZ-08 | 21-26 | 21,22,25 | 23,24 |
| HZ-09 | 27,28 | 27 | - |
| HZ-10 | 29,30,31 | 30,31 | 29 |
| HZ-11 | 32,33,34,35 | all | - |
| HZ-12 | 36,37,38 | all | - |
| HZ-13 | 39,40 | all | - |
| HZ-14 | 19,20,41 | all | - |
| HZ-15 | 42 | 42 | - |
| HZ-16 | 43,44,45 | all | - |
| HZ-17 | 46,47,48 | all | - |
| HZ-18 | 49,50 | all | - |
| HZ-19 | 51,52 | 51(logic) | 51,52 |
| HZ-20 | 53 | - | 53 |
| HZ-21 | 54,55 | - | 54,55 |

Items needing fw-security review: HZ-04, HZ-07, HZ-08, HZ-09, HZ-10, HZ-11; REQ-WMQ-27, 28, 29, 32.

## 4. Open questions for the user (hardware and requirement facts not found in the repo)

- Q1 What is the actual CI-150A streaming rate on CH1 (frames per second), and is it configurable on the indicator? The queue, heartbeat and soak budgets in HZ-01 are sized from the 9600 baud upper bound only.
- Q2 How is the MAX13487E driver direction controlled on CH1 (auto-direction, or a DE/RE GPIO)? Is the indicator's RS-485 port half duplex on the same pair? Needed before any transmit path (HZ-20) is allowed.
- Q3 Are flash encryption and secure boot enabled on deployed units? No sdkconfig in the tree matched FLASH_ENC or SECURE_BOOT; this decides whether NVS broker credentials are readable from flash (HZ-08, HZ-09).
- Q4 Is the ESP-IDF task watchdog enabled, and which tasks are subscribed? Not found in the tree (HZ-19).
- Q5 Does the deployed broker enforce per-device ACLs and TLS, and does the server use a distinct identity? Decides residual severity of HZ-10.
- Q6 What are the plant consequences of a frozen or shifted weight at the future pump (overfill volume, pressure, material hazard), and does the indicator have a setpoint relay output? Sets the severity and the hardware mitigation for HZ-21.
- Q7 What value is settings.transient_command_ttl_seconds in the deployed .env? It bounds how long a PENDING ZERO/TARE row can be replayed by startup recovery (HZ-12).
