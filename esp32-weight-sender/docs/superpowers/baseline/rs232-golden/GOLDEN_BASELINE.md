# RS232 Golden Baseline — captured BEFORE transport abstraction

Captured 2026-10-07, before any RS232→RS485 or transport-abstraction work.
This file is the frozen reference. The abstraction is only accepted if every
value below is reproduced unchanged afterwards.

Scope: `esp32-weight-sender` on the RS232 path. Downstream
(`esp32-relay-controller`, `server`) is recorded as "must not change" and was
not modified.

---

## 1. UART configuration (frozen)

From `components/drivers/uart_driver.c` and `main/Kconfig.projbuild`.

| Parameter | Value | Source |
|---|---|---|
| Data bits | 8 | `UART_DATA_8_BITS` hardcoded |
| Parity | none | `UART_PARITY_DISABLE` hardcoded |
| Stop bits | 1 | `UART_STOP_BITS_1` hardcoded |
| Flow control | none | `UART_HW_FLOWCTRL_DISABLE` hardcoded |
| Source clock | `UART_SCLK_DEFAULT` | `uart_driver.c:33` |
| RX buffer | 1024 bytes | `SCALE_UART_RX_BUFFER_SIZE` |
| TX buffer | 0 (none) | `uart_driver_install(..., 0, ...)` |
| Event queue | 20 entries | `SCALE_UART_EVENT_QUEUE_SIZE` |
| CH1 UART / RX / TX / baud | UART1 / GPIO35 / GPIO33 / 9600 | `WEIGHT_DEMO_CH1_*` |
| CH2 UART / RX / TX / baud | UART2 / GPIO34 / GPIO32 / 9600 | `WEIGHT_DEMO_CH2_*` |
| Console UART | refused | `uart_driver_init` returns `ESP_ERR_INVALID_STATE` |
| RX pull-up | only if `GPIO_IS_VALID_OUTPUT_GPIO(rx)` | GPIO34/35 are input-only → warn, no pull-up |

Pins are the PCB-fixed map from `task.txt`: `232RX1=GPIO35`, `232TX1=GPIO33`,
`232RX2=GPIO34`, `232TX2=GPIO32`. GPIO34/35 are input-only.

---

## 2. Known-good CI-150A / CI-2001 frames

Wire format: 20-byte body + `CR LF` = 22 bytes. The reader strips CR/LF
before parsing; the parser sees exactly 20 bytes (`CI2001_FRAME_BODY_SIZE`).

Layout (from `cas_ci2001_parser.c:7-31`):

```
  0-1   status   US | ST | OL
  2     ','
  3-4   GS | NT
  5     ','
  6     device ID (raw byte 0..99; 0x0A = LF collision)
  7     lamp status octet (opaque, undocumented)
  8     ','
  9-16  weight   (8 bytes, right-aligned ASCII)
  17    pad (0x00 in the manual example)
  18-19 unit     kg | lb
```

Golden vectors live in `golden_frames.h` and are exercised by
`tests/qemu/main/test_transport_golden.c`. The boot self-test vector in
`cas_ci2001_parser_self_test()` is the reference shape:

```
'S','T',  ',', 'G','S', ',', 0x0A, 0x00, ',',
'0','0','1','0','.','0','0','0', 0x00, 'k','g'   + CR LF
```

→ `gross = 10.0`, `display = 10.0`, `stable = true`, `unit = "kg"`,
`raw_status = 0x00`, `device ID = 10` (the LF-collision case).

### Parser outputs frozen

| Frame | gross | net | display | stable | overload | unit | display_net | raw_status |
|---|---|---|---|---|---|---|---|---|
| `ST,GS,id=1, 0010.000kg` | 10.0 | — | 10.0 | true | false | kg | false | 0x00 |
| `US,GS,id=1, 0005.500kg` | 5.5 | — | 5.5 | false | false | kg | false | 0x00 |
| `OL,GS,id=1, 9999.999kg` | 9999.999 | — | 9999.999 | false | true | kg | false | 0x00 |
| `ST,NT,id=1, 0000.000kg` | — | 0.0 | 0.0 | true | false | kg | true | 0x00 |
| `ST,GS,id=10, 0010.000kg` | 10.0 | — | 10.0 | true | false | kg | false | 0x00 |
| `ST,GS,id=1,-001.500kg` | -1.5 | — | -1.5 | true | false | kg | false | 0x00 |
| `ST,GS,id=1, 0010.000lb` | 10.0 | — | 10.0 | true | false | lb | false | 0x00 |
| `ST,GS,id=1, 0007.250kg` lamp=0x2A | 7.25 | — | 7.25 | true | false | kg | false | **0x2A** |

Malformed inputs that must stay rejected (frozen negatives):

- wrong length (≠ 20 bytes)
- comma at offset 2/5/8 replaced
- status not in {US, ST, OL}
- gross/net not in {GS, NT}
- device ID > 99
- unit not kg/lb
- weight field with a non-digit/non-dot/non-leading-minus
- weight field with no digit

---

## 3. Link-state timing (frozen)

| Constant | Value | Source |
|---|---|---|
| ONLINE confirmation | 2 consecutive matching frames | `mark_valid_frame`, `candidate_count >= 2` |
| Confirmation window | 3000 ms | `CONNECTION_TIMEOUT_MS` |
| STALE threshold | 3000 ms | `CONFIG_WEIGHT_DEMO_CAS_STALE_MS` |
| OFFLINE threshold | 10000 ms | `CONFIG_WEIGHT_DEMO_CAS_OFFLINE_MS` |
| Reader poll | 20 ms | `event_wait = pdMS_TO_TICKS(20U)` |
| RX chunk | 128 B | `RX_CHUNK_SIZE` |
| RX drain budget | 512 B/loop | `RX_DRAIN_BUDGET` |
| RX recovery drain | 1024 B | `RX_RECOVERY_DRAIN_BUDGET` |
| Frame buffer | 256 B | `FRAME_BUFFER_SIZE` |
| Error log interval | 10000 ms | `ERROR_LOG_INTERVAL_MS` |
| Publish rate | 1 Hz | `transmit_task` |

State machine: `WAITING → ONLINE → STALE → OFFLINE`. START state is
`WAITING` (`CAS_LINK_WAITING`); starting the source never claims ONLINE.

---

## 4. Published weight JSON schema (frozen)

The live weight link is **WebSocket JSON**, not MQTT (MQTT is design-only).
Schema from `weight_build_json()`:

```json
{
  "type": "weight",
  "weight_kg": <int>,
  "weight_g": <int>,
  "source": "CAS_RS232",
  "channel": <uint>,
  "stable": true|false,
  "weight1_g": <int|null>,
  "stable1": true|false,
  "weight2_g": <int|null>,
  "stable2": true|false,
  "sequence": <uint>,
  "cas_seq": <uint>,
  "age_ms": <uint>
}
```

Frozen invariants:
- `source` is always the literal `"CAS_RS232"`.
- `weight_g` is authoritative grams; `weight_kg = grams/1000` rounded, **not clamped**.
- No `"simulated"` key ever. No `"source":"SIM"` ever.
- A non-scale sample (`from_scale == false`) is refused → builder returns 0.
- A buffer too small is refused → builder returns 0, never truncates.

---

## 5. Safety contract (frozen — must not change)

From `components/weight_source/include/weight_source.h:12-24`:

> If the CAS scale is silent, malformed or stale, `weight_source_get()`
> returns `WEIGHT_SOURCE_RESULT_NONE` and NOTHING is published. A substituted
> value would hide a real fault and could drive a dispense.

**Never convert invalid/stale scale data into a valid 0 kg reading.**

Relay-side failsafe (unchanged): `WEIGHT_MESSAGE_TIMEOUT_MS = 5000` →
`failsafe_off_except_manual()`.

---

## 6. Regression-test baseline (frozen)

| Suite | Command | Result at baseline |
|---|---|---|
| `esp32-weight-sender` QEMU | `tools/run_qemu_tests.ps1` (native PowerShell) | **82 pass / 0 fail** |
| parser boot self-test | `cas_ci2001_parser_self_test()` | pass (checked at boot) |

The QEMU suite is pure-logic: it covers `weight_source`, `provisioning`
AP-name builder, `websocket_server` no-client guards and the QEMU-only sim
helpers. It does **not** exercise `scale_reader` or `uart_driver` (those need
a UART). After the refactor it must still report **82 pass / 0 fail**, and the
new golden-replay tests must add to that count without disturbing any of the
82.

Downstream suites were recorded as must-not-change and were not modified:
- `esp32-relay-controller` QEMU
- `server` pytest

---

## 7. What is frozen vs. what may move

**Frozen (behavior must be bit-identical):**
frame assembly, CRLF stripping, `ci2001_device_id_is_lf`, all parser output,
`reading_is_plausible`, the 2-frame ONLINE rule, STALE/OFFLINE thresholds,
diagnostics field names and semantics, JSON schema and `source` literal,
`WEIGHT_SOURCE_RESULT_NONE` semantics, log wording that tests assert on.

**Allowed to move (structure only):**
where the UART calls live. `uart_read_bytes` / `uart_get_buffered_data_len` /
`uart_flush_input` / `uart_event_t` handling move behind a transport
interface. `uart_driver_init` stays as the RS232 init entry point. Pin/baud
handling stays compile-time in Kconfig.

**Forbidden in this phase (Phase 0 gate):**
any RS485 behavior, DE/RE GPIO assignment, 2-wire vs 4-wire assumption,
`UART_MODE_RS485_HALF_DUPLEX`, baud/parity/addressing guesses, termination
or bias values, any change to CI-150A framing.

---

## 8. Post-abstraction verification (2026-10-07)

Measured after the transport abstraction landed. Every frozen value above is
reproduced; nothing in sections 1–7 was edited to make a test pass.

| Suite | Command | Baseline | After | Verdict |
|---|---|---|---|---|
| `esp32-weight-sender` QEMU | `tools/run_qemu_tests.ps1` | 82 pass / 0 fail | **349 pass / 0 fail** | identical (82 retained, 267 new) |
| `esp32-relay-controller` QEMU | `tools/run_qemu_tests.ps1` | must not change | **676 pass / 0 fail** | unmodified, green |
| `server` pytest | `pytest tests` + `DATABASE_URL=sqlite://` | must not change | **162 passed** | unmodified, green |
| parser boot self-test | `cas_ci2001_parser_self_test()` | pass | pass | identical |

### Rollback profile

RS232 is the **sole and default** transport backend. `scale_transport_rs232.c`
is a thin adapter that calls the **unchanged** `uart_driver_init()`; `uart_driver.c`
/ `.h` were not modified. There is no RS485 code, no DE/RE reference, and no
RS485 config symbol anywhere in the tree. Restoring the pre-abstraction
behaviour is therefore the current build — no separate profile is required
today. When an RS485 backend is eventually added it must be **additive** behind
a Kconfig choice that keeps `WEIGHT_DEMO_SCALE_TRANSPORT_RS232` selectable in
the same commit; that choice is deliberately not created now, because a
one-option choice is dead config and the Phase 0 gate forbids encoding
unverified RS485 values.

### A note on the FIFO-overflow path

The frozen reader drains the ring **before** counting dropped bytes, so a
good frame already queued behind an overflow is consumed by that drain and
does not survive as a separate reading. That is pre-existing behaviour, not a
refactor regression, and the golden tests assert it as such: the overflow is
counted, the partial produces no weight, `dropped_bytes` moves, and the next
complete frame is accepted.
