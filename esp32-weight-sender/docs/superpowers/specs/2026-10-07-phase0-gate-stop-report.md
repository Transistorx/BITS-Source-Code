# Phase 0 Gate — Stop Report

**Date:** 2026-10-07
**Scope:** Phase 2 "transport abstraction on RS232", per the approved migration
plan and the executor's binding conditions.
**Status:** **STOPPED at the Phase 0 hardware-verification gate.** No RS485
behaviour was implemented, and none may be until V1–V17 are closed.

---

## 1. What was changed

Everything below is structural. No application-level behaviour was altered.

### 1.1 New — transport contract

| File | Role |
|---|---|
| `components/interfaces/scale_transport.h` | The seam. `scale_transport_ops_t` vtable: `open`, `wait_event`, `read`, `flush_input`, `get_buffered_len`, `pending_events`, `close`. Plus `scale_xport_event_type_t` and `scale_link_config_t`. |
| `components/interfaces/scale_transport.c` | Null-safe convenience wrappers. Every failure path is inert — 0 bytes, 0 pending events, ignored flush — so a silent link publishes **nothing** rather than inventing a weight. |

`scale_link_config_t` deliberately carries **only** `uart_port`, `rx_gpio`,
`tx_gpio`, `baud_rate`. There is no `de_gpio`, no `rs485_mode`, no device
address. Those would encode Phase-0-forbidden hardware guesses.

### 1.2 New — RS232 backend (the rollback implementation)

| File | Role |
|---|---|
| `components/drivers/scale_transport_rs232.h/.c` | Wraps the **unchanged** `uart_driver_init()`. Translates `uart_event_t` → `scale_xport_event_t`. Preserves the negative `esp_err_t` from `uart_read_bytes` so `uart_read_errors` still counts identically. |

`components/drivers/uart_driver.c` and `uart_driver.h` were **not modified**.

### 1.3 Modified — reader rewired onto the contract

| File | Change |
|---|---|
| `components/scale_hal/scale_reader.h` | `scale_reader_config_t` takes `scale_transport_t *transport` instead of `int uart_port` / `QueueHandle_t uart_event_queue`. Drops `driver/uart.h`. |
| `components/scale_hal/scale_reader.c` | `drain_uart` → `scale_transport_read`. `handle_uart_event` takes `scale_xport_event_t`. Task loop uses `scale_transport_pending_events` / `wait_event`. `scale_reader_start` no longer does UART_NUM_MAX / console-UART checks (now the backend's job). |

**Byte-identical and untouched:** frame assembly, CRLF stripping,
`ci2001_device_id_is_lf`, `parse_scale_frame`, `mark_valid_frame`, the
two-frame ONLINE rule, every diagnostics counter and log string.

### 1.4 Modified — build wiring

`components/scale_hal/CMakeLists.txt` no longer depends on `drivers` or
`esp_driver_uart`. `components/interfaces` gained `scale_transport.c`.
`components/drivers` gained `scale_transport_rs232.c` and now depends on
`interfaces`. `main/app_main.c` allocates `s_rs232_transport[]` and hands the
reader `&link->base`.

### 1.5 New — golden baseline and equivalence tests

| File | Role |
|---|---|
| `docs/superpowers/baseline/rs232-golden/GOLDEN_BASELINE.md` | Frozen record: UART config, frame layout, parser output table, link timing, JSON schema, safety contract, test baseline. |
| `docs/superpowers/baseline/rs232-golden/golden_frames.h` | Header-only vectors. 8 golden frames + 8 frozen malformed negatives. |
| `tests/qemu/main/mock_transport.h/.c` | In-memory transport implementing the ops vtable, with instrumentation counters. |
| `tests/qemu/main/test_harness.h` | Shared `test_check` sink. |
| `tests/qemu/main/test_transport_golden.c` | The equivalence suite (5 sections, below). |
| `tests/qemu/main/test_main.c` | Calls `test_transport_golden_run()`. |

---

## 2. Behaviours proven identical

Proven by replaying the **same captured RS232 frames** and asserting the
outputs frozen in `GOLDEN_BASELINE.md` before and after the seam.

### 2.1 Parser output — same frames, same fields

All 8 golden frames through `cas_ci2001_protocol.parse_frame_with_length`,
asserting `valid`, `has_gross`, `has_net`, `has_display`, `stable`,
`overload`, `gross`, `net`, `display`, `unit`, `raw_status` — 11 fields × 8
frames. Includes the hard cases:

- **Device ID 10 = `0x0A` = LF collision** — survives the framing layer intact.
- **Negative weight** `-1.500` kg.
- **lb unit**.
- **Opaque lamp octet `0x2A`** preserved in `raw_status`.

All 8 frozen malformed bodies stay rejected, plus NULL-safety on both
pointer arguments.

### 2.2 Externally observable stream behaviour

Driven through the **real `scale_reader` task** over a mock transport:

- First valid frame emits **nothing** (candidate only).
- Second frame yields exactly one ONLINE + one READING.
- Once online, **exactly one READING per frame**, no redundant ONLINE.
- Every READING matches the frozen golden field-for-field.
- Malformed frame → **no READING**, link not dropped.
- Link recovers: the next good frame is accepted.

### 2.3 Transport faults carry through, never fabricate

- FIFO overflow → counted, partial emits **no weight**, `dropped_bytes`
  moves, next complete frame recovers and parses to golden values.
- Frame error / BREAK → counted, **no weight emitted**.
- NULL and unopened transports are inert (0 bytes, no event).

### 2.4 Application contract untouched

The 82 pre-existing checks all still pass, including `json_source_is_cas_rs232`,
`arbitration_labels_source_cas_rs232`, `reading_labels_source_cas_rs232`,
`no_scale_publishes_nothing`, and the `normalize_*` / `grams_*` /
`arbitration_*` / `json_*` / `cas_*` / `ap_name_*` groups. This proves
`weight_g` semantics, the JSON schema, the `"CAS_RS232"` literal, stale/offline
arbitration, and the NEVER-publish-a-0-kg-fallback rule are unchanged.

---

## 3. Tests run

| Suite | Command | Result |
|---|---|---|
| `esp32-weight-sender` QEMU | `tools/run_qemu_tests.ps1` (native PowerShell) | **349 pass / 0 fail** (was 82; 267 new) |
| `esp32-relay-controller` QEMU | `tools/run_qemu_tests.ps1` | **676 pass / 0 fail** — unmodified |
| `server` pytest | `pytest tests` with `DATABASE_URL=sqlite://` | **162 passed** — unmodified |
| `cas_ci2001_parser_self_test()` | boot self-test | pass |

Relay-controller and server were **not touched** and are green.

---

## 4. Rollback verification

RS232 is the **sole and default** backend.

- `components/drivers/uart_driver.c` / `.h` — **unmodified**.
- `scale_transport_rs232.c` is a thin adapter that calls the unchanged
  `uart_driver_init()`.
- No RS485 code, no DE/RE reference, no RS485 Kconfig symbol exists in the tree.
- Same 8N1 config, same pins, same RX ring (1024), same console-UART refusal.

The rollback profile **is** the current build. A separate
`WEIGHT_DEMO_SCALE_TRANSPORT_RS232` choice was deliberately **not** created:
with only one backend it is dead config, and inventing an RS485 sibling now
would mean encoding unverified hardware values. When an RS485 backend is
added, it must be **additive** behind a Kconfig choice that keeps RS232
selectable **in the same commit**.

---

## 5. UNRESOLVED — V1–V17 must be closed before any RS485 work

No value below was invented. Every one is a site/hardware fact the repository
and manuals do not prove. **Phase 0 is a hard blocking gate.**

| ID | Question | Status |
|---|---|---|
| **V1** | Which indicator is deployed — CI-150A or CI-2001A/B? Electrical type differs (2-wire TRX vs 4-wire IN/OUT). | **OPEN** |
| **V2** | Is the RS-485/422 option board actually fitted? Standard hardware is RS-232 only. | **OPEN** |
| **V3** | 2-wire half-duplex or 4-wire full-duplex? | **OPEN** |
| **V4** | Configured output mode on the RS-485 port: Stream / stable-only / Command / Modbus? | **OPEN** |
| **V5** | Configured baud / data bits / parity / stop on the RS-485 port? | **OPEN** |
| **V6** | Configured Device ID (0–99)? | **OPEN** |
| **V7** | Does Stream Mode on RS-485 broadcast unsolicited, or only respond to poll? Determines whether TX/DE is needed at all. | **OPEN** |
| **V8** | Frame format on RS-485: 22-byte CAS ASCII, 10-byte, 18-byte AND, or Modbus registers? | **OPEN** |
| **V9** | Any checksum/CRC on the selected ASCII format? (Manuals show none; capture still required.) | **OPEN** |
| **V10** | ESP32 PCB: any RS-485 transceiver footprint/IC, or only MAX3232-class? | **OPEN** |
| **V11** | Free output-capable GPIO for DE — **only if TX is required**. No DE pin is documented anywhere in the weighing stack. | **OPEN** |
| **V12** | **Live baud: Kconfig says 9600, bench docs say 4800.** A real discrepancy in the repo. | **OPEN** |
| **V13** | Cable length / environment / noise — drives termination and bias. | **OPEN** |
| **V14** | Multi-drop intent: one scale now, more later? | **OPEN** |
| **V15** | Is a common GND available between scale and ESP32? (CI-2001 exposes GND; CI-150A pin map shows only P/N.) | **OPEN** |
| **V16** | Hazardous-area / isolation requirements? | **OPEN** |
| **V17** | Does the scale side already include internal termination? | **OPEN** |

### Explicitly NOT done, and must not be done until the gate opens

- No DE/RE GPIO assigned or guessed.
- No 2-wire vs 4-wire assumption.
- No `UART_MODE_RS485_HALF_DUPLEX` selection.
- No baud / parity / addressing assumption.
- No termination or bias values.
- No change to CI-150A framing or the `cas_ci2001_parser`.
- No Modbus weight register map — the manual does not give one.

---

## 6. What may proceed without the gate

Nothing that touches hardware. If further work is wanted before V1–V17 close,
it is limited to: more golden capture fixtures, diagnostics polish, or
documentation — all constrained to not encode RS485 values.

## 7. Recommendation

Close **V2** first. If the RS-485 option is not fitted, the migration is a
hardware change, not a firmware change, and everything downstream is moot.
Close **V1/V3** next (they fix the electrical topology), then **V12**
(the 4800-vs-9600 discrepancy is a live silent-link risk regardless of RS485).
