# ESP32 #2 - Relay Controller

WebSocket client that receives two independent weight channels, runs a
**bounded global priority queue**, and controls two independent dispense
channels. Material M1 is fixed to Channel 1 / Pump 1 / Relay 1 P6 / Scale 1 /
PID 1; Material M2 is fixed to Channel 2 / Pump 2 / Relay 2 P7 / Scale 2 /
PID 2. The ESP32 assigns each global-queue job only to its compatible channel
and keeps each channel's PID history independent. It fails safe to ALL-OFF on loss of communication,
malformed data, stale weight, fault, timeout, overshoot, emergency stop or
watchdog failure.

## Architecture

```text
weight_receiver: parse, validate, freshness and link failsafe
  ├─ Scale 1 ──> dual_dispense_controller CH1 PID ──> Relay 1
  └─ Scale 2 ──> dual_dispense_controller CH2 PID ──> Relay 2
                         ▲
                 global priority/FIFO queue
                         │
  local web_api ── queue and operator controls
  telemetry_client ── run upload, device status, command polling,
                      target-profile configuration staging
  WSL server ── durable commands, versioned profiles, history and UI

relay_driver ── safety_manager ── PCF8574T @ 0x22 (Relay 1=P6, Relay 2=P7)
```

The WSL server never switches relays or runs the real-time PID loop. It may
change material display labels, but cannot change the hardware association.
The ESP32 validates remote material identity and profile configuration; queued
jobs are assigned only to the fixed compatible channel and profile changes take
effect only between jobs.

## Confirmed relay hardware

Relay 1 and Relay 2 are **not** direct ESP32 GPIOs. They are two outputs of a
single **PCF8574T** I2C GPIO expander:

| | Value | Evidence |
|---|---|---|
| 7-bit I2C address | **0x22** | A2=1, A1=0, A0=1 → `0b0100_101` |
| Relay 1 (coarse) | **P6**, bit 6, mask `0x40`, IC pin 11 | confirmed |
| Relay 2 (fine) | **P7**, bit 7, mask `0x80`, IC pin 12 | confirmed |
| I2C bus | SDA = GPIO22, SCL = GPIO23 | unchanged from the existing project |

The address is written in `relay_expander.h` as the actual OR
(`BASE | (A2<<2) | (A1<<1) | A0`) so the derivation is visible in the source
rather than a magic number, and the QEMU suite pins it.

Pins P0..P5 are **not** relay outputs on this board. They belong to whatever
else shares the expander, so every write is a shadow-preserving
read-modify-write seeded from a live bus read: the unused bits are handed back
verbatim. A blind byte write would clobber them.

### Active level — MEASURED: ACTIVE-HIGH

**ACTIVE-HIGH. OFF clears bits 6 and 7; ON sets them.**

This was measured on the physical board, and it **supersedes an earlier
derivation that was wrong**. The firmware first shipped ACTIVE-LOW, derived from
two plausible-sounding readings:

1. A PCF8574 output is quasi-bidirectional with a weak pull-up, so it sinks but
   cannot source — therefore the drive stage must be sink-driven and the
   energized level must be LOW.
2. The device powers up with every port pulled HIGH, and an active-HIGH board
   would energize its relays at power-up before the MCU booted, which no
   shipped product does.

Both steps are sound in the abstract. Both failed here. On the bench the relay
was **ON whenever it should have been OFF and OFF whenever it should have been
ON** — a complete inversion, which is the signature of an ACTIVE-HIGH drive
stage being written with active-LOW levels. The lesson, recorded rather than
quietly deleted: step 1 was an inference about a circuit nobody had traced, and
step 2 assumed a design norm instead of looking at the board in front of us.

**Consequence — read this before connecting AC loads.**

- The PCF8574 powers up with every port HIGH. Under ACTIVE-HIGH that is the
  **energized** level, so the relays are ON between power-up and the firmware's
  first OFF write. The firmware closes that window as early as it can
  (`relay_expander_init()`, called from `relay_init_all()` before Wi-Fi, the web
  API and the supervisor) but **cannot make it zero**. Removing it needs a
  hardware change, for example pull-downs on the relay-drive inputs.
- If the expander never ACKs, there is **no way to clear that state from
  firmware at all**. The driver says so loudly at ERROR level
  (`report_expander_unreachable()`), naming the relays as possibly energized.

`WEIGHT_DEMO_RELAY_POLARITY` remains a Kconfig choice so a board variant can
still invert it, and MAPTEST/PCF_TEST remain available to confirm the polarity
physically.

The safety design is unchanged and still sound: **OFF is never gated**, so every
boot and fault path can issue it unconditionally. Under ACTIVE-HIGH that write
is load-bearing rather than a formality — clearing an already-clear bit is a
no-op, and clearing a set bit can only ever de-energize.

### Bench verification after flashing

Both the address and the polarity were corrected from field measurements, and
the two interact — so the boot log, not this document, is what settles it. Flash
the fixed build and read back three things.

**(a) The I2C scan — what actually ACKs.** Every device that answers is logged:

```
I relay_expander: I2C scan: device ACK at 0xNN
I relay_expander: relay expander PCF8574T detected at 0x22 | Relay1=P6(0x40 pin 11) Relay2=P7(0x80 pin 12)
```

If 0x22 does not appear, the expander is not being seen and everything below is
moot — fix the bus first.

**(b) The backend selection line — which path is driving the valves.**

```
I relay_driver: relay backend: I2C expander PCF8574T 0x22 (active-HIGH) | Relay1=P6(coarse) Relay2=P7(fine)
```

or, if the expander was not reached:

```
W relay_driver: relay backend: direct GPIO (expander 0x22 unavailable) - Relay 1/2 are NOT on the I2C path
W relay_driver: *** dosing channels 1/2 are running on the DIRECT-GPIO FALLBACK, whose polarity is UNVERIFIED on this board ***
```

This line is the discriminator for the inversion report. With nothing ACKing at
the old (wrong) address, the backend disabled itself and `relay_set()` fell back
to GPIO18/19 — so the relays that were seen to move may have been driven by that
fallback, not by the expander. If the fallback line is the one you see, the
inversion was a property of the AO drive stage and **this whole section's
polarity applies to the wrong path**.

**(c) Which relay clicks, in which phase — MAINS DISCONNECTED.** Run
**ACT_TEST** (`WEIGHT_DEMO_I2C_ACT_TEST=y`): it walks both relays and prints an
expectation-vs-actual table including a live pin readback. MAPTEST
(`WEIGHT_DEMO_I2C_MAP_TEST=y`) remains available for the full per-bit mapping
walk. Both are deliberately not gated by `WEIGHT_DEMO_POLARITY_VERIFIED`.

In the shipped build the gate is already on, so a queued ramp job will also
open the valves — but run ACT_TEST first and read the table, because it is the
only step that tells you the pins obeyed the write *and* which relay clicked.

| Observation | Conclusion |
|---|---|
| Both relays energize on the ACTIVE-level step / when the job opens them, and release at target | The shipped ACTIVE-HIGH polarity is correct for this path |
| The inverse | Flip `WEIGHT_DEMO_RELAY_POLARITY` and re-run — the choice exists for exactly this |
| Relay 1 responds to a bit other than P6 (or Relay 2 to other than P7) | The P6/P7 mapping needs correcting; MAPTEST's per-bit walk names which bit moved which relay |

Only after (a) shows 0x22, (b) shows the I2C backend, and (c) agrees with the
configured polarity should AC loads be connected.

## Dispensing

### Job queue

`job_queue` is a bounded FIFO of up to `WEIGHT_DEMO_JOB_QUEUE_MAX` (8) records.
Each record stores `id`, `target_g`, `state`, `requested_ms`, `start_ms`,
`start_g`, `finish_ms`, `final_g` and an `error` string.

States: `QUEUED` → `RUNNING` → `COMPLETE` / `CANCELLED` / `FAILED`.

Records are **retained** after a job finishes so the API and the log can report
what actually happened; a finished record is the slot a later `add()` reuses,
which keeps the store a fixed, statically allocated array — no heap churn in
the middle of a dispense. `add()` refuses an out-of-window target
(`ESP_ERR_INVALID_ARG`) and a genuinely full queue (`ESP_ERR_NO_MEM`).

The FIFO order is by insertion sequence, and the controller starts the next
queued job automatically when one finishes.

### State machine

```
IDLE ──queue non-empty──► LOAD_JOB ──► WAIT_FOR_SCALE
                                           │ fresh weight, below target
                                           ▼
                                     COARSE_DISPENSE ──near target──► FINE_DISPENSE
                                                                          │ in tolerance
                                                                          ▼
                       ◄──NEXT_JOB── COMPLETE ◄──in tolerance── SETTLING
                                                       │ short of target
                                                       ▼
                                              FINE_CORRECTION ──► SETTLING
```

`FAULT` and `EMERGENCY_STOP` are reachable from every state; both close every
valve and are latched by the safety manager.

`LOAD_JOB`, `COMPLETE` and `NEXT_JOB` are bookkeeping steps rather than
physical waits, so they are re-dispatched inside a **single** tick. Without
that, each hop would burn a 250 ms supervisor period and the handover to the
next queued job would visibly stall.

### Coarse and fine

**Far below target both valves open** for the fast fill. Once the remaining
distance falls to `WEIGHT_DEMO_COARSE_TRANSITION_G` the coarse relay is closed
and the fine relay takes over alone.

Relay 1 = coarse = logical index 0 = expander bit 6.
Relay 2 = fine = logical index 1 = expander bit 7.

Every relay command in the dispense path is issued from exactly **one** place —
a single `switch` inside `dispense_controller_tick()` — so there are no
scattered conditionals that can disagree about which valve should be open.

### Fine control: PID + time-proportional output

A solenoid is a binary device, so the controller never tries to write an analog
percentage to it. The PID produces a **duty cycle in [0,1]** and
`dispense_window_output()` realises that duty as a fraction of a fixed window:

```
on_ms = duty x WEIGHT_DEMO_WINDOW_MS
```

PID runs **only in the fine region**. The coarse phase is a plain binary
full-open fill. Gains are integers in Kconfig scaled by 10000
(`WEIGHT_DEMO_PID_KP_X10000 = 25` means `0.0025`), with an anti-windup clamp on
the integral.

**Anti-chatter is structural, not a timer.** A burst shorter than
`WEIGHT_DEMO_MIN_ON_MS` is dropped entirely — it would not move the needle and
every transition wears the solenoid. A gap shorter than
`WEIGHT_DEMO_MIN_OFF_MS` is eliminated by holding the valve open for the whole
window. Either way the valve sees at most one transition per window.

The minimum-ON burst also sets a floor on what the loop can resolve: one burst
moves the vessel by `(MIN_ON_MS/1000) x fine_rate`, so
`WEIGHT_DEMO_TOLERANCE_G` must stay above that or the fine phase could only
overshoot. The defaults satisfy this (40 ms × 200 g/s = 8 g, against a 20 g
tolerance). The integral term then works the residual down below the
proportional deadband — which is exactly what it is for.

### Safety manager

`SAFETY_NONE` · `WEIGHT_LINK_LOST` · `WEIGHT_STALE` · `WEIGHT_DISCONNECTED` ·
`OVERSHOOT` · `TIMEOUT` · `EMERGENCY_STOP` · `CONTROL_FAILURE`

Raising any fault de-energizes **every** relay immediately and holds the
controller in `FAULT`. Clearing policy differs deliberately between two groups:

- **Transient** (`WEIGHT_LINK_LOST`, `WEIGHT_STALE`, `WEIGHT_DISCONNECTED`)
  describe the LINK, not the machine. They auto-clear when a fresh valid weight
  arrives, and the running job is **parked**, not destroyed — a two-second Wi-Fi
  blip must not throw away a half-dispensed vessel. The job budget keeps
  running, so a link that never recovers still fails the job rather than
  leaving it `RUNNING` forever.
- **Latched** (`OVERSHOOT`, `TIMEOUT`, `EMERGENCY_STOP`, `CONTROL_FAILURE`)
  each mean the machine did something an operator needs to look at. Silently
  resuming a dispense into an over-filled vessel is the failure this module
  exists to prevent. They need an explicit clear, and an emergency stop is
  *released* rather than cleared so a generic button cannot restart dispensing.

The **control watchdog** is fed by the dispense controller each tick and
checked *after* it, so a controller that stopped advancing is detected rather
than reported healthy by its own heartbeat.

## Relays OFF at every trigger

| Trigger | Path |
|---|---|
| Boot / reset | `relay_init_all()` pre-drives the AO nets LOW, then `relay_expander_init()` issues the **early OFF write** to 0x22 — before Wi-Fi, the WebSocket client or the supervisor exist |
| Wi-Fi loss | `on_wifi_lost()` → link-lost failsafe + `WEIGHT_LINK_LOST` fault |
| Stale weight | supervisor tick → `ALL RELAYS -> OFF (fail-safe)` + `WEIGHT_STALE` |
| Missing weight | same failure with no reading ever held (`WEIGHT_DISCONNECTED`) |
| Fault | `safety_manager_raise()` calls `relay_all_off()` on every raise |
| Timeout | job budget expiry → job failed + `TIMEOUT` fault |
| Overshoot | beyond `WEIGHT_DEMO_MAX_OVERSHOOT_G` → job failed + `OVERSHOOT` fault |
| Emergency stop | valves closed, running job failed, queue cancelled, latched |
| Watchdog failure | `CONTROL_FAILURE` fault → all relays OFF |

The 250 ms supervisor re-asserts all-OFF whenever no valid message is held.
`relay_expander_all_off()` is **transition-only**, so once the state matches
this is completely silent on the bus.

`relay_expander_all_off()` is deliberately **not** gated on
`WEIGHT_DEMO_POLARITY_VERIFIED`, unlike the ON path. The inactive level is the
expander's own power-up state under the confirmed polarity, so the write cannot
energize anything and every fault path can issue it unconditionally.

### Ramp path (RAMP sender)

With the sender's independent ramp the same control path runs, but the reading
climbs on its own. The job still has to reach its target, close both valves and
COMPLETE - inside the 5 s hold window when the target is the top of the ramp -
and the next queued job then waits for a fresh cycle at a confirmed 0 g. That
is what makes a queued-job run repeatable.

## Web UI and API

An HTTP server on `WEIGHT_DEMO_WEB_API_PORT` (default 80) on this device's
static address. Every route accepts **GET and POST**, so the whole surface is
reachable from a browser address bar and from `curl` without a body.

The control page takes the target in **kilograms** with a
`0 - Normal Queue (FIFO)` / `1 - Priority` dropdown, and shows the simulated
weight, simulation cycle (plus an *awaiting 0 kg* marker while the gate is
closed), Weight Sender link and freshness, active target, PID
setpoint/PV/error, control output, both relay states, progress, the queue with
its priority column, and the fault state.

| Route | Effect |
|---|---|
| `GET /` | Control page: live weight, target, progress, state, valve states, scale link, fault, queue table, and buttons |
| `GET /api/status` | Full state as JSON (the poll target) |
| `POST /api/job` | `target_kg=5.0` (plus optional `priority=0` or `1`) -> queue a job. `target_g` and `target` are still accepted for compatibility |
| `POST /api/cancel` | `id=N` → cancel a queued job; no id → cancel all queued |
| `POST /api/pause` | Hold the running job with both valves closed |
| `POST /api/resume` | Resume from `WAIT_FOR_SCALE` (re-checks freshness first) |
| `POST /api/estop` | Emergency stop |
| `POST /api/clear` | Clear a latched fault / release an e-stop |
| `POST /api/clear_jobs` | Cancel the running job and clear the queue |
| `POST /api/sim` | `source=real\|sim\|auto`, `sim=static\|dynamic\|progression` — forwarded **upstream** to the weight sender |

`/api/status` reports weight, target, progress, active job id and state, queue
depth and contents (with priority), relay states, duty, corrections, scale
connection status and age, the last message's source, the simulation cycle and
reset count, whether the zero-gate is holding, the PID setpoint/PV/error, the
safety fault, and the sender link state.

The target is validated against the accepted window (`WEIGHT_DEMO_MAX_TARGET_G`,
default 20 kg, matching the simulator's ramp top). A request outside it is
refused with 400 rather than becoming a job that can never complete.

The API **submits jobs to the queue and issues controller commands** — it never
touches a relay GPIO. A request from the web therefore cannot bypass a fault
latch or leave a valve open. `/api/sim` composes the sender's own command frame
and puts it on the wire rather than reaching into the sender's state, and its
parameters are restricted to plain tokens so a request cannot inject JSON into
a frame handed to another device.

## Bench test mode: the deterministic ramp

The authoritative bench setup runs the sender's independent 0 to 20 kg ramp
rather than the valve-coupled vessel. Two things change on this side, and both
are **explicit, logged selections** - never inferred from the payload, so a
real scale cannot inherit the relaxed behaviour by sending a field.

### Completion semantics (`WEIGHT_DEMO_COMPLETION_MODE`)

| Mode | Behaviour |
|---|---|
| `RAMP_TEST` (default) | target/tolerance reached -> both relays OFF -> COMPLETE; later **rising readings are ignored for that job** |
| `PROCESS` | the real-machine rules: settle, judge, overshoot beyond the limit is a fault |

`RAMP_TEST` exists because the ramp keeps climbing after the valves close. Under
`PROCESS` rules every job would be failed with *"overshoot after settling"* for
behaviour the controller caused correctly. The strict rules are not weakened
anywhere - they are selected per mode, and `PROCESS` is what a real scale uses.

The "target already satisfied at start -> instant COMPLETE" shortcut is
**disabled** in `RAMP_TEST`: a job that finds itself above target fails with
*"started above target"* rather than reporting a success that dispensed nothing.

### Starting a job (confirmed 0 g)

In `RAMP_TEST` a job starts only at a **confirmed 0 g reading**, and a job
queued after another finished waits for a **fresh simulation cycle** as well:

```
I RELAY_CTRL: waiting for next 0-kg cycle | weight=18000 g | reset_seen=0 | queue=2 | priority=1
```

This gate is checked in *both* places a job can start - `IDLE` and `NEXT_JOB` -
because a finished job chains `COMPLETE -> NEXT_JOB -> LOAD_JOB` inside a single
tick and a gate living only in `IDLE` would never be consulted on the automatic
handover, which is exactly the path the requirement is about.

Starting at an arbitrary point in the ramp would hand a job a part-filled scale
and could report a completed job that never dispensed anything.

The value recorded for a completed job is the reading **at target-reached**, not
at `COMPLETE`: under the ramp those differ by however much the simulator climbed
during the settle window.

### Simulation cycle resets

```
W RELAY_CTRL: SIMULATION_CYCLE_RESET | cycle=3 | reading restarted at 0 g - deliberate simulator reset, NOT a fault
```

Detected primarily from the sender's `simulation_cycle` field changing, with the
deliberate top-of-ramp to 0 transition as a secondary signal for a sender that
reports `simulated` without a cycle number. It is **never** treated as a
physical fault, and it never re-enables a completed job's relays.

## Priority queue

`priority 0` is the normal FIFO queue; `priority 1` is high priority.

- The running job is **non-preemptive** - a priority-1 arrival never interrupts
  it. The selector only ever looks at `QUEUED` jobs, so this holds by
  construction rather than by a check.
- Among waiting jobs every priority-1 job runs before any priority-0 job.
- Within one priority level the order is plain FIFO by insertion.

`job_precedes()` is a pure, strictly-ordered predicate and is unit-tested on its
own, so the ordering rule is pinned independently of the storage.

## End-to-end simulated run

### Closed-loop path (DYNAMIC sender)

With the sender in its `DYNAMIC` mode the weight rises because the valves open,
so the full physical loop is exercised:

```
set a 5 kg target in the UI  →  job_queue  →  dispense state machine
   →  relay 1 + relay 2 open (I2C 0x22, bits 6 and 7)
   →  the sender's DYNAMIC virtual vessel fills
   →  weight packets come back over the same WebSocket
   →  coarse closes at the transition distance, fine modulates
   →  tolerance reached, both valves closed, SETTLING
   →  COMPLETE  →  NEXT_JOB  →  the next queued job starts on its own
```

The weight rises **because the state machine actually opened the valves** — it
is not a constant 5 kg. For the sender's virtual scale to follow the valves,
the controller publishes its valve state back over the same WebSocket link
(`{"type":"relay_state","coarse":…,"fine":…,"reset":…}`); `reset` marks a job
boundary so the sender empties its vessel for the next job. Publishing is
decoupled from the control loop through a small queue, so a socket can never
stall a valve decision.

The QEMU suite drives exactly this loop — a virtual vessel filled only by the
commands the controller issues, fed back through the real message path, at the
real cadences (100 ms control ticks, 1 Hz weight publication) — and asserts the
job completes within tolerance, that the coarse→fine handover happened, that
the fine valve was observed **closed while still in the fine region** (the
signature of time-proportional output), and that a second job then runs to
completion unattended.

## Build

```
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py fullclean build
idf.py -p <PORT> flash monitor
```

A clean build is required after adding or removing a component directory: an
incremental build can silently omit a new component.

### Partition table

`partitions.csv` gives the application a **3 MB** partition. The default
"single app" layout allows only 1 MB, which this firmware had filled to 97%
(3% free, with a build warning) - a boot failure waiting for the next feature.
The sender uses the same layout.

## Configure (provisioning)

Wi-Fi credentials are stored in NVS and set over the air. No credential appears
in any source file or in menuconfig.

1. Hold the **BOOT button (GPIO0) for 3 s**. The device reboots into
   provisioning mode and broadcasts an open access point named
   **`BITS-Relay-XXXX`**, where `XXXX` is the last four hex characters of the
   unique device id, upper-cased (e.g. device id `bits-a4cf12ab34cd` →
   `BITS-Relay-34CD`). The weight sender derives its own name the same way, so a
   pair always reads as `BITS-Scale-34CD` / `BITS-Relay-34CD`.
2. Join that network from a phone or laptop; a captive portal opens
   automatically (or browse to `http://192.168.4.1`).
3. Pick your network from the scanned list, enter the password and press
   **Save & Restart**. Credentials are written to NVS and the device reboots
   into normal operation.

Until credentials are stored, Wi-Fi is skipped entirely at boot (no radio, no
retry loop) and the log prints the "needs provisioning" hint once.

### Provisioning reset

Holding BOOT for **10 s** erases the stored credentials from NVS
(`nvs_config_erase_all()`) *before* the portal opens, then reboots. This is
deliberately different from the 3 s hold — that one only re-opens the portal
and overwrites the slot, so walking away without submitting the form leaves the
old credentials in place and the device silently rejoins the previous network.
Both thresholds use the existing BOOT button.

## Configuration

`idf.py menuconfig` → **Weight WebSocket Bench Test (relay controller)**.

| Option | Default | Meaning |
|---|---|---|
| `WEIGHT_DEMO_STATIC_IP` | `192.168.137.76` | This device's static IPv4 (gateway `192.168.137.1`, mask `255.255.255.0`) |
| `WEIGHT_DEMO_WEIGHT_SERVER_IP` | `192.168.137.217` | **Primary** target: the weight sender's static IP — normal operation does not depend on mDNS |
| `WEIGHT_DEMO_WEIGHT_SERVER_PORT` | `80` | Sender's WebSocket port |
| `WEIGHT_DEMO_WS_PATH` | `/ws` | WebSocket URI path |
| `WEIGHT_DEMO_WEIGHT_SERVER_MDNS_NAME` | `weight-sender` | Diagnostic-only fallback, used when the server IP is left empty |
| `WEIGHT_DEMO_CONTROL_MODE` | `DISPENSE` | `DISPENSE` (queue + state machine, relays 1/2) or `LEVEL_GROUP` (legacy: all 12 relays at the trigger) |
| `WEIGHT_DEMO_TARGET_G` | `5000` | Initial dispense target (5.000 kg) |
| `WEIGHT_DEMO_TOLERANCE_G` | `20` | Settled weight accepted as done |
| `WEIGHT_DEMO_COARSE_TRANSITION_G` | `1000` | Close coarse this far from target |
| `WEIGHT_DEMO_SETTLE_MS` | `1500` | Both valves closed before judging |
| `WEIGHT_DEMO_MAX_DURATION_MS` | `120000` | Whole-job budget, waiting included |
| `WEIGHT_DEMO_MAX_OVERSHOOT_G` | `100` | Beyond this over target is a fault |
| `WEIGHT_DEMO_CORRECTION_LIMIT` | `5` | Max fine-correction attempts per job |
| `WEIGHT_DEMO_MAX_TARGET_G` | `20000` | Largest accepted target |
| `WEIGHT_DEMO_JOB_QUEUE_MAX` | `8` | Job records retained |
| `WEIGHT_DEMO_WINDOW_MS` | `500` | Fine-control PWM window |
| `WEIGHT_DEMO_MIN_ON_MS` / `_MIN_OFF_MS` | `40` / `40` | Anti-chatter bounds |
| `WEIGHT_DEMO_PID_KP/KI/KD_X10000` | `25` / `3` / `1` | PID gains, fine region only |
| `WEIGHT_DEMO_PID_INTEGRAL_MAX` | `200` | Anti-windup clamp |
| `WEIGHT_DEMO_CONTROL_WATCHDOG_MS` | `3000` | Control-loop heartbeat limit |
| `WEIGHT_DEMO_WEB_API_PORT` | `80` | Web control/API port |
| `WEIGHT_DEMO_MESSAGE_TIMEOUT_MS` | `5000` | Stale-data failsafe window |
| `WEIGHT_DEMO_TRIGGER_G` | `10000` | `LEVEL_GROUP` mode only: group threshold in grams |
| `WEIGHT_DEMO_I2C_SDA` / `_SCL` | `22` / `23` | I2C bus to the PCF8574T at 0x22 |
| `WEIGHT_DEMO_RELAY_POLARITY` | `ACTIVE_HIGH` | Expander output polarity — **measured** on the board (see above) |
| `WEIGHT_DEMO_POLARITY_VERIFIED` | `y` | Gates ON commands only; shipped **on**, on measured evidence. OFF is never gated |
| `LED_STATUS_GPIO` | `-1` | Status LED disabled on this board — see below |

### Actuation gate — ON, on measured evidence

ON commands require `WEIGHT_DEMO_POLARITY_VERIFIED=y`, and **the shipped build
sets it**, in `sdkconfig` and `sdkconfig.defaults` alike. The justification is
measurement rather than assumption: the mapping (PCF8574T at 0x22, Relay 1 =
P6/0x40/pin 11, Relay 2 = P7/0x80/pin 12) and the ACTIVE-HIGH polarity are both
field-measured on this board.

**The gate mechanism is untouched.** Setting it back to `n` still suppresses ON
with zero bus traffic, which is what a board variant or an unverified board
would want; nothing about the mechanism was weakened to make the dosing valves
move.

OFF is never gated in any configuration, so boot, Wi-Fi loss, stale weight,
fault, timeout, overshoot, emergency stop and the watchdog all de-energize
regardless of this setting.

### Startup actuation block

One block at boot answers the only question an operator has when a valve will
not move — which backend is live, and is ON permitted? Every input to that
decision is printed, including the *compiled* gate value, so the answer is read
rather than inferred:

```
I relay_expander: --- relay actuation state ---
I relay_expander:   expander detected      : yes
I relay_expander:   backend active         : yes
I relay_expander:   configured polarity    : ACTIVE-HIGH (1 = energized)
I relay_expander:   WEIGHT_DEMO_POLARITY_VERIFIED (compiled) : 1
I relay_expander:   ON commands allowed    : yes
I relay_expander:   OFF commands           : always allowed (never gated)
I relay_expander:   shadow byte now        : 0x3f (relay bits CLEARED)
```

### Per-command actuation trace

Every command on a dosing channel is attributed and traced: what was asked,
which backend carried it, and — when it was refused — why. A rejected ON that
left the reported state looking OPEN is the failure this trace exists to make
impossible to miss.

```
I relay_driver:   relay 1 -> ON via I2C expander 0x22
I relay_expander: REL ON idx0 (bit6) ACCEPTED | addr=0x22 shadow 0x3f -> 0x7f | write=0x7f | ESP_OK
I relay_expander: I2C relay write addr=0x22 value=0x7f (ESP_OK)
```

Refusals are never rate-limited away, and they name the reason:

```
W relay_expander: REL ON idx0 (bit6) REJECTED | WEIGHT_DEMO_POLARITY_VERIFIED=0 - zero bus traffic, shadow stays 0x3f
W relay_driver:   relay 1 -> ON via direct GPIO (fallback)  ...
E RELAY_CTRL:     coarse valve command REJECTED (ESP_ERR_INVALID_STATE) - valve is still CLOSED, not OPEN
```

That last line is the reporting contract: **the controller reports the valve
state the driver ACCEPTED, never the one it asked for.** The web UI, the status
JSON and the periodic control line all read the accepted state, so a refused
command can no longer be displayed as an open valve.

### ACT_TEST (ON-path walk — MAINS DISCONNECTED)

Set `WEIGHT_DEMO_I2C_ACT_TEST=y`, **disconnect mains/load**, flash, boot. After
a 5 s countdown it walks the two dosing relays through every combination,
printing per step the expected wire byte, the byte written, the `esp_err` and a
**plain readback of the live pin state** (a PCF8574 read returns the pins), then
a self-judging summary table:

| Step | Expected | Meaning |
|---|---|---|
| both OFF (start) | `0x3f` | both relay bits cleared |
| RELAY 1 ON | `0x7f` | bit 6 set |
| RELAY 1 OFF | `0x3f` | |
| RELAY 2 ON | `0xbf` | bit 7 set |
| RELAY 2 OFF | `0x3f` | |
| both ON (final walk) | `0xff` | both bits set, then back to `0x3f` |

Those values assume P0..P5 read HIGH; the table prints the bytes actually
computed for the board, so unused pins pulled low show up as a different (and
equally correct) pattern. ACT_TEST is deliberately **not** subject to
`WEIGHT_DEMO_POLARITY_VERIFIED` — proving the pins obey is what verification is
for, exactly as with PCF_TEST and MAPTEST.

**How to read it:** bytes MATCH and the relay clicks on its step ⇒ that relay is
driven by this expander and the polarity is right. Bytes MATCH but nothing ever
clicks ⇒ the expander is not driving these relays (escalate with the I2C scan
list). Any MISMATCH ⇒ the pins are externally driven, or the device is not a
PCF8574.

### MAPTEST (mapping/polarity verification)

Set `WEIGHT_DEMO_I2C_MAP_TEST=y`, **disconnect mains**, flash, boot. After the
5 s countdown: Phase 1 drives both relays to the ACTIVE level for 3 s (prompt:
*energized? ⇒ polarity confirmed*) then to the INACTIVE level, ending
de-energized; Phase 2 walks all eight outputs one at a time (2.5 s each,
baseline restored between) to confirm or correct the expected
P6 = Relay 1 / P7 = Relay 2 assignment. The six non-relay bits are left at
their live-read values and never poked.

### PCF_TEST (write+readback discriminator)

Set `WEIGHT_DEMO_I2C_PCF_TEST=y`, **disconnect mains**, flash, boot. Walks the
inactive baseline then each single-relay bit on 0x22: write, immediate readback
(PCF8574 reads return live pin state), paired log, `MISMATCH` warning, 2.5 s
observation prompt. The two steps that should energize Relay 1 and Relay 2 are
labelled.

| Observation | Conclusion |
|---|---|
| Relays click per bit | Mapping confirmed → acceptance run |
| Readbacks match, nothing clicks | Device accepts writes but drives no relay → escalate with the full `I2C scan: device ACK at 0xNN` inventory |
| Any readback mismatch | Pins externally driven → same escalation |

Multimeter the actual PCF8574T **pin** (not the SSR output) during a
commanded-active step: <0.5 V = pin obeys; 4-5 V = externally driven.

### Static IP

The DHCP client is **stopped** and the static lease applied with
`esp_netif_set_ip_info()` before the radio connects, so the interface never
holds a DHCP lease alongside the static address. After `GOT_IP`, the firmware
reads the **actual** interface address back with `esp_netif_get_ip_info()`,
verifies it against the configuration (warns on mismatch) and logs:

```
I RELAY_CTRL: Static IP configured: 192.168.137.76
I RELAY_CTRL: Wi-Fi connected | IP=192.168.137.76
```

## Status LED: disabled on this board (GPIO2 = AO12)

GPIO2 is **AO12 / Relay 12** in the reference schematic and `relay_driver` is
its single owner. The `led_status` component is compiled in but disabled via
`CONFIG_LED_STATUS_GPIO=-1`: `led_status_init()` returns
`ESP_ERR_NOT_SUPPORTED`, no task is created, the pin is never claimed twice (so
IDF logs no `gpio: conflict found for GPIO[2]`), and the all-relays-OFF
failsafe genuinely holds AO12. Provisioning/connect hints are **log-only** on
this device. The weight sender — which has no relay outputs — keeps the GPIO2
onboard LED.

## Reference schematic AO map (fallback backend, do not reassign)

The direct-GPIO path is kept as the automatic fallback for board variants that
drive the AO nets and for channels 3..12, which are not on the confirmed
expander. On this board **indices 0 and 1 are never driven as GPIOs while the
expander backend is active**, so Relay 1 and Relay 2 are not remapped onto the
AO nets.

| Relay | GPIO | | Relay | GPIO |
|---|---|---|---|---|
| 1 AO1 | 18 | | 7 AO7 | 32 |
| 2 AO2 | 19 | | 8 AO8 | 33 |
| 3 AO3 | 21 | | 9 AO9 | 4 |
| 4 AO4 | 25 | | 10 AO10 | 13 |
| 5 AO5 | 26 | | 11 AO11 | 12 |
| 6 AO6 | 27 | | 12 AO12 | 2 |

`RELAY_ACTIVE_LEVEL 1` — GPIO HIGH energizes on that path: each AO line drives
an S8050 NPN which drives a G3MB-202PL solid-state relay.

## Board resources (reserved, do not repurpose)

| Resource | Pins | Notes |
|---|---|---|
| I2C | SDA = GPIO22, SCL = GPIO23 | Carries the PCF8574T relay expander at 0x22 (Relay 1 = P6, Relay 2 = P7) |
| RS232 | LRX = GPIO16, LTX = GPIO17 | Serial interface |
| Analog | A1 = GPIO36, A2 = GPIO39 | Input-only pads, ADC1 |

## Boot order

`relay_init_all()` drives every AO net LOW before configuring the pads as
outputs, then `relay_expander_init()` probes the bus read-only, chip-gates the
device and issues the early OFF write to 0x22. Both run before Wi-Fi or the
WebSocket client start. No relay can energize during initialisation.

## Message validation

Accepted only when the payload is a flat JSON object with `type == "weight"`
and a weight in either field:

- `weight_g` — integer grams, **authoritative** for the decision;
- `weight_kg` — rounded convenience field (fraction allowed when `weight_g`
  is absent; converted to grams by rounding).

The gram-domain rule matters: 9.5 kg rounds to `weight_kg: 10` on the wire, so
a kg-based decision would read it as 10 kg; `weight_g: 9500` does not.
Accepted window is 0..100000 g (0 kg from an idle scale is valid data: it
refreshes the timestamp and leaves the valves where the state machine puts
them). Rejected: negatives, absurd values (> 100 kg), fractional `weight_g`,
NaN/inf (e.g. `1e999`), strings, nulls, wrong/missing type, nested objects or
arrays — the scanner fails closed, and a rejected message cannot move the
timestamp or a relay. Rejection warnings are rate-limited to one per 30 s (with
a running total) so a peer streaming bad data cannot flood the console.

Unknown fields (`source`, `channel`, `stable`, `sequence`) are ignored by the
rule; `source` is captured for the log line and for `/api/status`.

## Failsafes

| Condition | Result |
|---|---|
| Boot | every relay OFF (AO pre-drive LOW + explicit early OFF write to 0x22) |
| Wi-Fi lost | all OFF, then reconnect (bounded, no reboot) |
| WebSocket lost | all OFF, then client auto-reconnect (bounded, no reboot) |
| No valid message for 5000 ms | all OFF, `Weight data timeout` + `ALL RELAYS -> OFF (fail-safe)` |
| Invalid message | rejected; timestamp and relays both untouched |
| No valid message held (any reason) | all-OFF re-asserted every 250 ms tick |

The relays stay OFF until a NEW valid message arrives, including after a
reconnect. The 250 ms re-assertion closes the cross-task race between an
in-flight `weight_receiver_on_message()` (WebSocket task) and a link-loss event
(Wi-Fi task): even if the interleaving momentarily leaves a group energized with
no valid message held, the next supervisor tick forces it off (covered by the
`tick_heals_group_on_without_valid_message` QEMU test).

The device never reboots because the peer is temporarily unavailable: the
WebSocket client retries with bounded 2 s backoff, and if the client could not
start at all (no network yet), a task retries every 15 s.

## Legacy LEVEL_GROUP mode

`WEIGHT_DEMO_CONTROL_MODE=LEVEL_GROUP` restores the previous bench behaviour
exactly: all 12 relays act as one group, at or above `WEIGHT_DEMO_TRIGGER_G`
energizes and below it drops out. The job queue and the state machine are
disabled; the failsafes are identical. It is kept so the earlier 10 kg bench
acceptance sequence remains reproducible without reflashing a different
firmware — it is not the default.

## Logic tests (no hardware needed)

```
.\tools\run_qemu_tests.ps1
```

Two environment gotchas that cost time if rediscovered: `qemu-system-xtensa`
must be on `PATH` (prepend `C:\Espressif\tools\qemu-xtensa\bin`), and from
Git Bash the variables `MSYSTEM`, `MSYS` and `MINGW_*` must be unset or
`idf.py` aborts with *"MSys/Mingw is no longer supported"* before doing any
work. The same two notes apply to the sender's suite.

485 checks. Runs as real firmware under QEMU. QEMU does not emulate Wi-Fi and has no I2C
slaves, so this suite covers validation, the gram-domain group decision (the
8.00 OFF / 10.00 ON / 12.00 hold-ON / 9.50 OFF acceptance sequence), timeout
behaviour, sequence diagnostics, the failsafe race closure, payload
reassembly, and — for the dispensing architecture — the confirmed expander
mapping (address derivation to 0x22, masks `0x40`/`0x80`, `bit_for_index`,
shadow helpers with unused-bit preservation, the boot fail-safe pattern), the
job queue (window, FIFO order, bounded depth, terminal transitions, snapshot
ordering, cancellation), the PID and the windowed output including the
anti-chatter rules, the full state machine, overshoot / stale / missing-weight
/ timeout / e-stop / watchdog behaviour with both DOSING relays asserted OFF at
each, and the end-to-end closed loop described above.

Added for the bench ramp mode: parsing of the additive `simulated` /
`simulation_cycle` / `stable` fields (with legacy frames still accepted and a
malformed declared field failing closed); the priority rule and the queue
policy including non-preemption and FIFO within a level; that `get_running()`
returns a caller-owned copy; the 1 / 5 / 10 / 20 kg targets completing with both
relays observed ON together when far below, OFF at target, and a coarse-to-fine
handover; the 20 kg job completing inside the hold with no freshness fault; that
later rising readings and the 20-to-0 reset neither restart a completed job nor
raise a fault; that the next queued job starts only at a confirmed 0 g on a
fresh cycle; that a job queued mid-ramp is never reported as a zero-dose
success; and that `PROCESS` mode still applies the strict overshoot rules.

The WebSocket path, static IP, the I2C bus and the real relay outputs are
hardware-verified.

## Dispensing telemetry uploader

The telemetry client is a separate lower-priority network task; relay switching
and PID execution stay in the supervisor. Validated weight messages are copied
to a bounded FreeRTOS queue with a zero-wait send. The telemetry task uploads
per-channel runs, samples, state events and device status, and polls durable WSL
commands. For each received server job it fetches the selected target profile
for both channels and stages the validated configuration before adding the job
to the ESP32 queue. The controller applies a matching staged profile only at a
channel job boundary. The uploader filters samples marked `simulated`, batches
samples and keeps a 48-sample per-run ring with a drop-oldest policy. Queue and
ring drops are counted and warned at a limited rate. Network failures cannot
wait in the supervisor, scale parser, or relay path.

Remote pause, cancel, emergency stop and clear requests are polled, so their
response includes network and polling delay. The ESP32's local operator controls
remain available for immediate operation; no browser or WSL request writes
relay pins directly.

Configure from the relay-controller project with `idf.py menuconfig`:

| Kconfig setting | Value |
|---|---|
| `WEIGHT_DEMO_TELEMETRY_SERVER_IP` | Windows laptop LAN IPv4 from `ipconfig`; empty disables uploads. Do not use WSL's `172.x` IP. |
| `WEIGHT_DEMO_TELEMETRY_SERVER_PORT` | `8000` (or the server's configured port). |
| `WEIGHT_DEMO_TELEMETRY_API_KEY` | Same secret as `API_KEY` in `server/.env`. |
| `WEIGHT_DEMO_TELEMETRY_DEVICE_ID` | Optional override; empty uses the NVS device ID. |

Keep the default `DISPENSE` control mode. The runtime accepts only
non-simulated validated scale messages for samples. Real machine tuning should
use completion mode `PROCESS`. With telemetry configured, serial logs report
the server URL, run registration, batch sizes, retries, buffer occupancy,
drops, and completion. Device-side buffering is volatile RAM: a reboot loses
buffered-but-unacknowledged telemetry, but never affects dispensing. Server,
Wi-Fi, and MySQL outages do not disable or pause the controller.

After configuration, rebuild and flash with the ESP-IDF commands for this
workspace. Verify first with loads disconnected: confirm `/health` is
reachable from the LAN and serial logs show run registration and uploaded
batches, then perform a supervised physical CAS dispense and confirm the run,
raw samples, and events in History. Do not use simulated ramp data for PID
tuning.

## Serial monitor

Every accepted weight prints one INFO line; state transitions print one extra
line; everything else is state changes only (Wi-Fi/WebSocket connect-loss,
link state, boot progress). The raw JSON stays at DEBUG level.

```
I RELAY_CTRL: boot | fw cycle6-dispense ...
I RELAY_CTRL: control mode: DISPENSE
I relay_expander: I2C bus up: SDA=GPIO22 SCL=GPIO23 @ 100 kHz
I relay_expander: relay expander PCF8574T detected at 0x22 | Relay1=P6(0x40 pin 11) Relay2=P7(0x80 pin 12)
I relay_expander: I2C relay write addr=0x22 value=0xc0 (ESP_OK)
I relay_driver: relay backend: I2C expander PCF8574T 0x22 (active-HIGH) | Relay1=P6(coarse) Relay2=P7(fine)
I RELAY_CTRL: Wi-Fi connected | IP=192.168.137.76
I RELAY_CTRL: Web control/API started on port 80
I RELAY_CTRL: Weight WebSocket connected
I RELAY_CTRL: Job 1 queued | target=5000 g | depth=1
I RELAY_CTRL: Dispense IDLE -> LOAD_JOB
I RELAY_CTRL: Job 1 RUNNING | target=5000 g | start=0 g
I RELAY_CTRL: Dispense LOAD_JOB -> WAIT_FOR_SCALE
I RELAY_CTRL: Coarse valve -> OPEN (fine=OPEN)
I RELAY_CTRL: Dispense WAIT_FOR_SCALE -> COARSE_DISPENSE
I RELAY_CTRL: Coarse valve -> CLOSED (fine=OPEN)
I RELAY_CTRL: Coarse -> fine handover | remaining=800 g
I RELAY_CTRL: Dispense COARSE_DISPENSE -> FINE_DISPENSE
I RELAY_CTRL: Dispense FINE_DISPENSE -> SETTLING
I RELAY_CTRL: Job 1 COMPLETE | target=5000 g | final=4991 g | corrections=0
I RELAY_CTRL: Dispense NEXT_JOB -> IDLE
```

### Liveness probe (mute-link self-healing)

The client sends a WebSocket PING every 5 s and requires a PONG within 10 s.
An open-but-mute socket is therefore detected within ~10 s: the client tears
down (failsafe: all relays OFF), auto-reconnects with bounded 2 s backoff, and
the fresh handshake re-arms the sender. Neither device reboots because of a
peer issue.

### Retry visibility (a failing link is never silent)

- Every disconnect/retry warning names the target URI, so a wrong address is
  obvious at a glance.
- A never-connected retry chain logs its 1st and every 30th attempt — the same
  rate-limit pattern as the Wi-Fi manager.
- After the client is started, the connect task stays alive as a slow
  supervisor: while disconnected it logs `still trying ws://... (client
  auto-reconnect active)` once every 30 s.

## Strapping pins

GPIO2 (AO12) and GPIO12 (AO11) are ESP32 boot-strapping pins. GPIO12 selects
the flash regulator voltage at boot and must not be pulled high externally.
Through the 4.7k base resistor the S8050 presents a diode to ground, which
pulls both pins low — the safe direction at boot — but the boot-time base
current is not characterised. Straps are sampled at reset only. Firmware never
enables pulls on the AO pins and pre-drives them LOW before switching pads to
outputs. The BOOT button (GPIO0) is configured as input+pull-up at runtime,
after the strap sample.

Note: GPIO2/4/12/13/25/26/27 are ADC2 channels — ADC2 is unusable while Wi-Fi
is active, so no analog monitoring should ever be added on AO-adjacent pins.
