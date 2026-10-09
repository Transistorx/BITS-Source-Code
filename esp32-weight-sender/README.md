# ESP32 #1 - Weight Sender

Reads a physical **CAS CI-150A** weighing scale over RS-232 and publishes
**only genuine parsed CAS frames** over WebSocket on `/ws`. There is no
simulator and no source-mode choice in production: every published frame is a
real CAS reading, tagged `source=CAS_RS232`, with a CAS sample sequence
(`cas_seq`) and an age stamp (`age_ms`). When the scale is silent, stale or
malformed, **nothing is published** — no fallback, no last-known-as-fresh. Holds
the static address `192.168.137.217`; the relay controller (`192.168.137.76`)
targets it directly.

Firmware version string: **`weight-sender 7.0-cas-physical`**.

## Build

```
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py fullclean build
idf.py -p <PORT> flash monitor
```

A clean build is required after adding or removing a component directory: an
incremental build can silently omit a new component.

## Configure (provisioning)

Wi-Fi credentials are stored in NVS and set over the air — the same flow as the
UART weighing-scale gateway. No credential or address appears in any source
file or in menuconfig.

1. Hold the **BOOT button (GPIO0) for 3 s**. The device reboots into
   provisioning mode and broadcasts an open access point named
   **`BITS-Scale-XXXX`**, where `XXXX` is the last four hex characters of the
   unique device id, upper-cased (e.g. device id `bits-a4cf12ab34cd` →
   `BITS-Scale-34CD`). The relay controller derives its own name the same way
   from the same id, so the two boards in a pair always read as
   `BITS-Scale-34CD` / `BITS-Relay-34CD`.
2. Join that network from a phone or laptop; a captive portal opens
   automatically (or browse to `http://192.168.4.1`).
3. Pick your network from the scanned list, enter the password and press
   **Save & Restart**. Credentials are written to NVS and the device reboots
   into normal operation.

Until credentials are stored, Wi-Fi is skipped entirely at boot (no radio, no
retry loop) and the status LED fast-blinks as the "needs provisioning" hint.

### Provisioning reset

Holding BOOT for **10 s** is a full reset: the stored credentials are erased
from NVS (`nvs_config_erase_all()`) *before* the portal opens, then the device
reboots. This is deliberately different from the 3 s hold — that one only
re-opens the portal and overwrites the slot, so walking away without
submitting the form leaves the old credentials in place and the device
silently rejoins the previous network. The 10 s hold guarantees it comes up
unprovisioned.

Both thresholds run on the existing BOOT button; no extra hardware is needed.

`idf.py menuconfig` -> **Weight WebSocket Bench Test (weight sender)**
centralizes every address and behaviour constant:

| Option | Default | Meaning |
|---|---|---|
| `WEIGHT_DEMO_STATIC_IP` | `192.168.137.217` | This device's static IPv4 (gateway `192.168.137.1`, mask `255.255.255.0`) — the relay controller's primary target |
| `WEIGHT_DEMO_WS_PORT` | `80` | HTTP server port carrying the WebSocket endpoint |
| `WEIGHT_DEMO_WS_PATH` | `/ws` | WebSocket URI path |
| `WEIGHT_DEMO_TELEMETRY_SERVER_IP` | `192.168.137.1` | Windows host address; the sender POSTs its own status to FastAPI `POST /api/v1/device/status` every 5 s |
| `WEIGHT_DEMO_TELEMETRY_SERVER_PORT` | `8000` | FastAPI port for the device-status POST |
| `LED_STATUS_GPIO` | `2` | Status LED (module onboard blue LED; this board has no relay outputs) |
| `WEIGHT_DEMO_MDNS_HOSTNAME` | `weight-sender` | Diagnostic convenience only — normal operation uses the static IP |
| `WEIGHT_DEMO_CAS_STALE_MS` | `3000` | CAS link STALE threshold |
| `WEIGHT_DEMO_CAS_OFFLINE_MS` | `10000` | CAS link OFFLINE threshold |
| `WEIGHT_DEMO_CAS_LOG_PERIOD_MS` | `1000` | Serial diagnostic rate limit |
| `WEIGHT_DEMO_TEST_SIMULATION` | `n` | TEST ONLY: compiles pure simulator helpers for QEMU host tests; never enable in production |
| `WEIGHT_DEMO_BOARD_VARIANT` | `RS485_MAX13487E` | Selects the one authoritative pin map in `components/board/include/board_pins.h` |
| `WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL` | `1` | Which RS-485 channel carries the single physical CAS CI-150A (deployed unit is wired to CH1/UART1). NVS key `scale_ch` (1/2, 0 = Kconfig) overrides at boot; the choice is logged. Weight, freshness, link state, health counters, ZERO/TARE and MQTT follow only this channel; there is no auto-switching |
| `WEIGHT_DEMO_CH1_BAUD` / `WEIGHT_DEMO_CH2_BAUD` | `9600` / `9600` | Per-channel baud (CAS CI-150A stream rate) |
| `WEIGHT_DEMO_LOG_SCALE_FRAMES` | `n` | Raw scale-frame logging (very verbose) |

GPIO and UART-peripheral numbers are **not** Kconfig values. They live only in
`components/board/include/board_pins.h`, so they cannot be scattered or set to
an invalid combination.

### Static IP

The DHCP client is **stopped** and the static lease applied with
`esp_netif_set_ip_info()` before the radio connects, so the interface never
holds a DHCP lease alongside the static address. After `GOT_IP`, the firmware
reads the **actual** interface address back with `esp_netif_get_ip_info()`,
verifies it against the configuration (warns on mismatch) and logs:

```
I WEIGHT_TX: Static IP configured: 192.168.137.217
I WEIGHT_TX: Wi-Fi connected | IP=192.168.137.217
```

## Status LED (GPIO2)

| Pattern | Meaning |
|---|---|
| Fast blink (5 Hz) | Provisioning portal active, or no credentials stored |
| Slow blink (1 Hz) | Wi-Fi connecting / reconnecting |
| Solid on | Wi-Fi connected (IP acquired) |
| Off | LED service not running |

The pin is a Kconfig choice: `CONFIG_LED_STATUS_GPIO=-1` disables the LED
service entirely (`led_status_init()` returns `ESP_ERR_NOT_SUPPORTED`, no task,
no pin claim). This board keeps GPIO2 — it has no relay outputs. On the
12-relay controller board the same GPIO2 is AO12/Relay 12, so **that** project
sets `-1` and `relay_driver` owns the pin; see its README.

## Scale channels / board pin map

One authoritative mapping lives in `components/board/include/board_pins.h`.
Nothing else in the firmware hardcodes a scale UART pin; callers fill a
`scale_link_config_t` from `board_pinmap_fill_link()`.

### RS-485 board (current, MAX13487E AutoDirection)

| RS485 Channel | ESP32 UART | TX GPIO | RX GPIO | DE/RE GPIO | Connector | A pin | B pin | GND pin |
|---|---|---|---|---|---|---|---|---|
| CH1 | UART1 | GPIO13 (`VFD_TX`) | GPIO15 (`VFD_RX`) | *none — AutoDirection* | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB |
| CH2 | UART2 | GPIO32 (`VFD_TX_2`) | GPIO33 (`VFD_RX_2`) | *none — AutoDirection* | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB |

UART0 (GPIO1 TX / GPIO3 RX) is reserved for console and flashing;
`uart_driver_init` refuses to configure it.

**Direction control is entirely hardware.** The MAX13487E drives DE/RE from its
own TX activity, so the ESP32 uses an ordinary full-duplex UART. There is **no**
ESP32 DE/RE GPIO, **no** RTS direction pin, and `UART_MODE_RS485_HALF_DUPLEX`
is **not** used. The only software consequence is that the transceiver may echo
TX into RX, which the UART backend flushes at the end of each write path. A
DE/RE pad on this board would be a wiring error, not a configuration option.

Connector / differential-pair numbering cannot be proven from any repository
file and is deliberately marked `VERIFY ON PCB` rather than guessed.

### Legacy RS-232 map (rollback board)

| RS232 Channel | ESP32 UART | TX GPIO | RX GPIO | DE/RE GPIO | Connector | A pin | B pin | GND pin |
|---|---|---|---|---|---|---|---|---|
| CH1 | UART1 | GPIO33 (`232TX1`) | GPIO35 (`232RX1`) | *n/a (RS-232)* | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB |
| CH2 | UART2 | GPIO32 (`232TX2`) | GPIO34 (`232RX2`) | *n/a (RS-232)* | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB | VERIFY ON PCB |

Select with `CONFIG_WEIGHT_DEMO_BOARD_RS232_LEGACY`. GPIO34/GPIO35 are
input-only (RX only) and have no internal pull-up — idle-high depends entirely
on a fitted MAX3232-class receiver.

### Compile-time guards

`board_pins.h` `_Static_assert`s reject, on every build of every variant:

- TX and RX sharing a pad
- both channels claiming the same UART peripheral
- either channel taking console UART0
- any scale pad colliding with console GPIO1/GPIO3
- an input-only pad (GPIO34–39) used as TX
- any pad reused across channels
- any scale pad colliding with an EMAC/RMII pad (17, 18, 19, 21, 22, 23, 25, 26, 27)
- `BOARD_HAS_DE_RE_GPIO` ever being defined on this board

### GPIO15 (MTDO) strap pad — Channel 1 boot/restart

On the RS-485 board Channel 1 RX sits on **GPIO15**, the MTDO strapping pad. A
receiver that drives the bus low during reset can disturb boot. At startup the
firmware reads the pad (report-only, never blocks boot) and logs its level.

**Bench verification** with the Channel 1 receiver:

1. **Connected** — receiver wired to CH1, bus idle-high at reset → boot normally.
2. **Idle** — receiver attached but no traffic during reset → boot normally.
3. **Actively streaming** — receiver driving the bus during reset → observe for
   boot disturbance.

If Channel 1 shows boot instability in **any** of those states, use **Channel 2
(GPIO32/GPIO33)** for the single scale. Do **not** silently remap pins to work
around a Channel 1 boot problem — switch channels. The PCB also needs a 10 kΩ
pull-up on GPIO15 to 3.3 V.

**Deployed channel (2026-10-09): Channel 1 / UART1 / TX GPIO13 + RX GPIO15** (hardware fact from the user; schematic `SCH_Schematic1_2026-10-09.pdf`). Channel 2 avoids the strap-pad hazard, but pins are never remapped and the channel is never switched automatically.

### Startup diagnostics

```
I board_pins: board variant=RS485_MAX13487E | active scale channel=CH2
I board_pins: RS485 CH1 | UART1 | TX=GPIO13 | RX=GPIO15 | DE=AutoDirection
W board_pins: RS485 CH1 RX is on GPIO15 (MTDO strapping pad): ...
I board_pins: RS485 CH2 | UART2 | TX=GPIO32 | RX=GPIO33 | DE=AutoDirection
I board_pins: connector=VERIFY ON PCB A=VERIFY ON PCB B=VERIFY ON PCB GND=VERIFY ON PCB (all VERIFY ON PCB)
I board_pins: GPIO15 (MTDO) strap pad level at boot = 1 (HIGH, normal boot)
```

### Single-scale deployment

Field deployment is **one** physical CAS CI-150A. `WEIGHT_DEMO_SCALE_ACTIVE_CHANNEL`
selects which RS-485 channel carries it (default `2`). Both channels are brought
up so the unused one can be diagnosed, but only the active channel is the
intended scale input. Channel 2 must **not** be treated as an independent second
weighing instrument — production publishes only genuine CAS frames from the
physical scale path.

Every parser in the stack is receive-only (`build_poll_request` is NULL), so the
TX pins are configured and owned by their UARTs but never transmit.

### These RX pins have no internal pull-up (legacy RS-232 only)

`gpio_set_pull_mode()` returns ESP_OK for GPIO34/GPIO35 while silently doing
nothing, because input-only pads have no pull capability and the function
discards the inner `gpio_pullup_en()` error. An idle-high RX therefore depends
entirely on a fitted MAX3232-class receiver. Without one those pins float and
produce break and noise errors rather than a clean idle line. The driver checks
the capability explicitly and logs a warning when the bias is unavailable.

## Message format

Every published frame is a genuine parsed CAS sample:

```json
{"type":"weight","weight_kg":10,"weight_g":9982,"source":"CAS_RS232","channel":1,"stable":false,"sequence":25,"cas_seq":412,"age_ms":85}
```

| Field | Notes |
|---|---|
| `weight_g` | exact normalised grams — **authoritative** for the receiver's threshold decision |
| `weight_kg` | rounded convenience value (`lround(g/1000)`), deliberately not clamped |
| `source` | always `CAS_RS232` in production |
| `channel` | 1 for the primary physical CAS input (2 only if channel 2 is the path that delivered the frame) |
| `stable` | CAS scale stability flag from the parsed frame |
| `sequence` | starts at 1, wraps to 1 after `UINT32_MAX`; only advances for messages actually transmitted |
| `cas_seq` | CAS sample sequence from the scale path; identifies the genuine CAS sample being published |
| `age_ms` | age stamp: how old the CAS sample is at publish time |

There is no `simulated` field and no simulator metadata: production frames are
always real CAS RS232 data.

The gram field is what matters: 9.5 kg rounds to `weight_kg: 10`, so a
kg-based decision would falsely trigger the receiver's 10 kg threshold. The
receiver reads `weight_g` (integer compare `>= 10000`) and treats `weight_kg`
as display-only. Values outside the receiver's accepted window (e.g. a negative
tare) are transmitted as-is and rejected there — the sender never clamps.

One message per 1000 ms, generated on a dedicated task. The sender samples the
latest **fresh** CAS reading at each tick; it does not forward every scale
frame. Nothing is generated or logged while no WebSocket client is connected,
so a reconnecting peer never sees a bogus sequence gap. When no fresh CAS data
exists, nothing at all is published (see
[CAS provenance / serial diagnostics](#cas-provenance--serial-diagnostics)).

## CAS frame freshness

Scale readings are normalised to grams (kg x1000, lb x453.59237) from genuine
CAS frames only. Freshness is tracked **per published field**, not per frame:
`scale_manager` merges fields across frames, so a status-only frame must not
make an old weight look fresh. `weight_source` stamps each candidate with the
update time of the field it actually publishes (`display_update_ms` or
`gross_update_ms`), and the candidate must still be inside the CAS link
thresholds (`WEIGHT_DEMO_CAS_STALE_MS` / `WEIGHT_DEMO_CAS_OFFLINE_MS`).

Outside that window the transmitter publishes **nothing**. There is no
fallback source, no last-known-as-fresh republish, and no simulated
substitute. The relay controller's stale-weight failsafe is the correct
response to a scale that is genuinely not reporting.

## CAS provenance / serial diagnostics

The CAS link is reported as an explicit state machine. Online is **never**
claimed before a valid frame has been parsed from the physical RS232 input.

```
CAS SCALE: WAITING FOR DATA
  -> CAS SCALE: ONLINE / RECEIVING REAL RS232 DATA
  -> CAS SCALE: STALE
  -> CAS SCALE: OFFLINE
```

Per-sample diagnostic line (one per new CAS sample sequence):

```
CAS SCALE: ONLINE | source=CAS_RS232 | weight=X.XXX kg | weight_g=N | seq=N | cas_seq=N | age=N ms | stable=YES/NO | valid=YES
```

CAS state lines print **only on a genuine state transition**. A steady
WAITING / STALE / OFFLINE condition never repeats the same serial line.

When no fresh CAS data exists, the transmitter publishes **nothing** and logs
the `no scale data` warning **once per state change** (not on a timer):

```
no scale data (source=CAS_RS232) - nothing published
```

That silence is deliberate. The relay controller's stale-weight failsafe is the
correct, safe response to a scale that is genuinely not reporting; substituting
any other value would mask a real fault. Runtime state transitions and log
timing require hardware verification against a live CI-150A.

## Reverse control channel

The relay controller is a WebSocket *client* of this server, and a WebSocket is
bidirectional — so it can publish diagnostic state back over the **same** link
that carries the weight. `ws_handler` dispatches inbound TEXT frames, but the
channel can **no longer** switch the weight source or enable a simulator: there
is only one production source (physical CAS RS232).

Only `relay_state` diagnostic frames are accepted:

```json
{"type":"relay_state","coarse":true,"fine":false,"reset":true}
```

Any `{"type":"cmd"}` frame (or any other command form that would change the
weight source or enable a simulator) is **refused loudly** — it is rejected and
logged as an error. The parser is fail-closed (nested objects and arrays are
rejected, unrecognised frames change nothing) and an inbound frame larger than
`WS_CMD_MAX_LEN` closes the connection rather than desynchronising the frame
stream.

## Serial monitor

Generation is never gated on client presence for diagnostics, but **weight
publication is gated on fresh CAS data**: with no valid frame, nothing is
published. `TX weight = ...` prints only for weights actually handed to the
WebSocket, and the sequence counter advances only on a successful send (a
reconnecting peer never sees a bogus gap). Diagnostics are **state changes
only** — client count on connect/disconnect, a `no WebSocket client` warning
on disconnect, CAS link state on transition, `no scale data` on transition,
and one provenance line per new CAS sample. Identical consecutive serial
lines are never repeated. The raw JSON stays at DEBUG level.

```
I WEIGHT_TX: boot | fw weight-sender 7.0-cas-physical
I NVS_CFG: device_id: bits-a4cf12ab34cd
I board_pins: board variant=RS485_MAX13487E | active scale channel=CH2
I board_pins: RS485 CH1 | UART1 | TX=GPIO13 | RX=GPIO15 | DE=AutoDirection
I board_pins: RS485 CH2 | UART2 | TX=GPIO32 | RX=GPIO33 | DE=AutoDirection
I board_pins: GPIO15 (MTDO) strap pad level at boot = 1 (HIGH, normal boot)
I scale_xport_rs232: RS485 link open: UART1 RX GPIO15 TX GPIO13 @ 9600 baud | DE=AutoDirection
I scale_xport_rs232: RS485 link open: UART2 RX GPIO33 TX GPIO32 @ 9600 baud | DE=AutoDirection
I WEIGHT_TX: Static IP configured: 192.168.137.217
I WEIGHT_TX: Wi-Fi connected | IP=192.168.137.217
I WEIGHT_TX: mDNS: weight-sender.local advertised
I WEIGHT_TX: Reporting device status to http://192.168.137.1:8000/api/v1/device/status as bits-a4cf12ab34cd
I WEIGHT_TX: WebSocket server started on /ws (port 80)
I WEIGHT_TX: Sender initialised
I WEIGHT_TX: active WebSocket clients = 0
I WEIGHT_TX: CAS SCALE: WAITING FOR DATA
W WEIGHT_TX: no scale data (source=CAS_RS232) - nothing published
I WEIGHT_TX: CAS SCALE: ONLINE / RECEIVING REAL RS232 DATA
I WEIGHT_TX: CAS SCALE: ONLINE | source=CAS_RS232 | weight=9.500 kg | weight_g=9500 | seq=1 | cas_seq=412 | age=85 ms | stable=YES | valid=YES
I WEIGHT_TX: WebSocket client connected | fd=54 (relay controller joins)
I WEIGHT_TX: active WebSocket clients = 1
I WEIGHT_TX: TX weight = 9.500 kg | source=CAS_RS232 | cas_seq=412 | age=85 ms | bytes=160
```

Device status is POSTed to FastAPI every 5 s (`SERVER_STATUS_PERIOD_MS`), well
under the server's 10 s device-online threshold. Runtime log cadence and state
transitions require hardware verification with a live CI-150A on the scale
input.

Client tracking on IDF v6.1: the URI handler is **never invoked for the WS
handshake itself** (`httpd_uri.c` sends the 101 and skips `uri->handler`), so
the client fd is registered in the `ws_post_handshake_cb` hook
(`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`) and continuously verified
against httpd's own session state via `httpd_ws_get_fd_info()` on every
transmit tick — a dropped peer is deregistered within ~1 s with a
`WebSocket client disconnected | fd=N` line. A plain non-upgrade GET on `/ws`
(e.g. a browser probe) receives HTTP 400 and never registers.

`TX weight` means the frame was **queued for send in the httpd task** — the
IDF-sanctioned context for `httpd_ws_send_frame_async` (calling it directly
from the transmit task races the httpd task's own PONG writes on the same
socket, which once failed the first broadcast and left a zombie link). The
sequence number advances only when the frame is accepted by httpd. If the
actual send then fails, the work function logs the reason
(`WebSocket send to fd N failed: ESP_ERR_... - closing socket`) and tears the
socket down with `httpd_sess_trigger_close`, so the relay sees a TCP close,
failsafes, and auto-reconnects — a mute-but-open link can no longer persist on
either side. No reboot is ever needed for a peer issue.

Wi-Fi reconnect retries are rate-limited (first failure, then every 30th) so
bad credentials cannot flood the console.

Raw scale frames are logged only when `WEIGHT_DEMO_LOG_SCALE_FRAMES` is enabled.
A CI-150A in stream mode sends about 22 frames/s per channel, so that option
floods the monitor at roughly 44 lines/s across both channels.

## Logic tests (no hardware needed)

```
.\tools\run_qemu_tests.ps1
```

Covers normalisation, rounding, channel arbitration, the JSON builder (exact
strings, `CAS_RS232` label, `cas_seq` / `age_ms` fields, buffer refusal),
per-field freshness selection, the clock-epoch match with `scale_manager`, and
the no-client guard paths.

`WEIGHT_DEMO_TEST_SIMULATION` (default `n`) compiles **pure simulator helpers
for QEMU host tests only**. Those helpers exercise arithmetic and parser
fixtures off-hardware; they are never enabled in production and never feed the
WebSocket. Do not turn this option on for a production image.

The suite also pins the `BITS-Scale-XXXX` derivation (truncation from the end
of the device id, upper-casing, short/absent/non-hex ids, buffer termination;
the relay controller's suite pins the identical function), inbound
`relay_state` parsing (accept and fail-closed on garbage / empty / nested /
unknown values — asserting a rejected frame leaves state unchanged), and that
`{"type":"cmd"}` is refused loudly without changing any weight-source state.

Wi-Fi, static IP, mDNS, the scale UARTs and the WebSocket server are
hardware-verified. End-to-end CAS publish / stale / offline transitions with a
live CI-150A require hardware verification.

### Running the tests

```
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd tests\qemu ; idf.py qemu
```

Two environment gotchas that cost time if rediscovered: `qemu-system-xtensa`
must be on `PATH` (prepend `C:\Espressif\tools\qemu-xtensa\bin`), and from
Git Bash the variables `MSYSTEM`, `MSYS` and `MINGW_*` must be unset or
`idf.py` aborts with *"MSys/Mingw is no longer supported"* before doing any
work.

## Known behaviour: an idle scale reads as rejected on the receiver

A connected scale sitting at 0.000 kg is valid data here and transmits
`weight_g: 0`. The relay controller accepts zero (relays OFF, timestamp
refreshed — the link stays healthy). Values outside its accepted window
(negative tare, > 100 kg) are rejected there; this sender deliberately does not
clamp, so the receiver sees the truth.

## Provenance

`components/{interfaces,scale_protocols,scale_hal,scale_manager,drivers}` are
**copied** from `esp32-uart-weighing scale/components/` so this project builds
and flashes standalone. That project is not modified by this work.

`components/{app_error,nvs_config,provisioning}` are also copied from the
weighing-scale gateway (NVS key table trimmed to the keys this project uses;
`provisioning` additionally drives the status LED), and `components/led_status`
is copied from `esp32-wifi-lan8270-2uart-vfd` — the real GPIO2 implementation,
not the scale's no-op stub — now with a Kconfig-selectable pin so the relay
controller can disable it (see the Status LED section).

The one deliberate change to the uart_driver copy: the RX pull-up is applied
only when the pin supports it, and a warning is logged otherwise (see above).

The copies can drift. Keep them in sync by hand.
