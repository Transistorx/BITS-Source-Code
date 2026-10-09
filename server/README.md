# Dispensing Telemetry Server (WSL / Ubuntu)

Operations, telemetry and tuning server for the ESP32 dispensing system. It
ingests weight samples, controller state and lifecycle events from the ESP32
Relay Controller and exposes a browser dashboard for queue operations, PID
profiles and test analysis. The ESP32 remains responsible for channel
assignment, PID execution, relay switching and safety. WSL sends durable job
and operator commands and stages profile configuration; losing FastAPI, MySQL
or Wi-Fi does not take over or interrupt the ESP32 control loop.

Data flow:

```
CAS scale -> 2-wire RS485 / MAX13487E -> ESP32 Weight Reader
  -> validated WebSocket weight packets (up to 10 Hz)
  -> ESP32 Relay Controller (PID / state machine / relays)
  -> independent HTTP live status (5 Hz) -> WSL FastAPI memory -> SSE -> browser
  -> separate buffered HTTP history -> WSL FastAPI -> MySQL -> historical graphs
```

## Operator pages and remote commands

The shared navigation exposes three primary pages: `/queue` (also `/`),
`/tuning`, and `/graphs`. The Queue page submits material, target weight and
priority to one global waiting queue. The ESP32 claims only compatible jobs
and runs one dispensing job at a time using one physical scale. An operator
confirms the scale position before the next nozzle takes ownership. The fixed association
is M1 / Material A -> CH1 -> Pump 1 -> Relay 1 / P6 -> Scale 1 -> PID 1, and
M2 / Material B -> CH2 -> Pump 2 -> Relay 2 / P7 -> Scale 2 -> PID 2. Operators
can rename material labels through `/api/v1/materials`; that endpoint cannot
change physical assignments. Scale 1/Scale 2 are logical channel labels; they
do not imply two simultaneously usable physical scales.

`app/mqtt_bridge.py` implements the MQTT topics in
[the contract](../docs/telemetry/CONTRACT.md) section 7. It is gated by
`MQTT_ENABLED` (default `false`); broker via `MQTT_BROKER_HOST` /
`MQTT_BROKER_PORT`, optional auth via `MQTT_USERNAME` / `MQTT_PASSWORD`.
Broker TLS is off by default; set `MQTT_TLS=true` and point `MQTT_BROKER_PORT`
at the broker TLS listener (usually 8883). The certificate and hostname are
always verified with TLS 1.2 minimum, against `MQTT_TLS_CA_FILE` (PEM, for a
private CA) or the system CAs when it is empty. A set but missing or unreadable
`MQTT_TLS_CA_FILE` stops startup with an error. The password is never logged.
HTTP stays the fallback command/history/health transport, and WebSocket
connects the two ESP32s. See [the integrated audit](../docs/integrated-audit/REPORT.md).

Live state is process-local: run Uvicorn with **one worker**. `/api/v1/live/status`
updates memory without DB access; `/api/v1/live/events` pushes coalesced SSE
at up to 10 Hz, and `/api/v1/live/telemetry` is the bounded polling fallback.
Proxy buffering must be disabled for SSE. The existing API-key header policy
applies; browser EventSource cannot supply an API-key header, so deployments
requiring authenticated reads need a same-origin session/proxy authentication
integration before enabling `READS_REQUIRE_KEY` for browser pages.

`/api/v1/live/ws` (sender weights and ZERO/TARE, `app/static/js/live_ws.js`)
follows the same read policy: with `READS_REQUIRE_KEY=true` and a non-empty
`API_KEY` the socket sends nothing until the client proves the key, either as
the `X-API-Key` header or as the first frame `{"type":"auth","api_key":...}`
within `LIVE_WS_AUTH_TIMEOUT_MS`; anything else closes the socket with 1008
before any snapshot. The live page prompts for the key once on that close.
A handshake with an `Origin` header is refused (1008) unless the origin host
equals the `Host` header or the origin is listed in `CORS_ORIGINS`; non-browser
clients without `Origin` are unaffected. `BENCH_OPEN_WRITES=true` only opens
commands to loopback clients (127.0.0.1, ::1). At most `MAX_WS_CLIENTS` sockets
are served; extra handshakes are refused with 1013 and the browser retries with
backoff. Run uvicorn with `--ws-max-size 1024` so oversize frames are dropped
by the server before buffering (the application limit is 512 bytes). The broker
side of the same hazard (HZ-10) is covered by the ACL template in
`deploy/mosquitto/README.md`.

Enable the MQTT bridge in `server/.env`:

```
MQTT_ENABLED=true
MQTT_BROKER_HOST=<broker LAN address>
MQTT_BROKER_PORT=1883
MQTT_USERNAME=bits_backend
MQTT_PASSWORD=<from the broker secrets file>
MQTT_TLS=false
MQTT_TLS_CA_FILE=
LIVE_WS_DEVICE_WHITELIST=<sender ids allowed ZERO/TARE>
MAX_WS_CLIENTS=16
BENCH_OPEN_WRITES=false
```

The code reads the host and port from `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT`.
Run a single worker with `--ws-max-size 1024`. Do not run `BENCH_OPEN_WRITES`
behind a reverse proxy: the proxy makes every client look like loopback.

WebSocket usage: connect to `ws://<host>:8000/api/v1/live/ws`, send
`{"type":"auth","api_key":"..."}` first when a key is required, then receive
live weight snapshots and updates; ZERO/TARE commands are accepted only for
ids in `LIVE_WS_DEVICE_WHITELIST`.

Known risks: the pump/controller depends on the network only and needs an
independent non-network stop. Device broker passwords are stored unencrypted in
NVS (flash encryption off).

On WSL Ubuntu, use the project virtual environment and existing `.env` values:

```bash
source .venv-wsl/bin/activate
python -m pytest tests -q
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000 --workers 1 --ws-max-size 1024
```

The MQTT bridge and the live command path require a single worker. With
WEB_CONCURRENCY above 1 the bridge refuses to start unless MQTT_CLIENT_ID is set.

Use the reachable LAN address already configured on the devices. A WSL NAT
address or localhost from the ESP32 is not the laptop's reachable LAN address;
verify the existing WSL forwarding/mirrored-network and firewall configuration
on the deployment machine. Do not substitute an invented broker address.
The browser never controls relay outputs directly. Remote cancel, pause,
resume, emergency stop and clear requests are stored as device commands and
polled by the controller; the local ESP32 web controls remain available when
the server is offline.

PID profile versions are immutable records scoped by material, its fixed
channel and optional target. Applying or reverting a version changes the
server's selected profile for the next matching material job. The ESP32
validates the configuration and activates it only at a channel job boundary; each run records the profile ID/version and
the gains actually used. The Graphs page groups the latest ten displayed runs
by material, channel and target without deleting older runs or samples; historical runs
remain available through filters and run IDs.

Operational endpoints are under `/api/v1`:

| Method | Path | Purpose |
|---|---|---|
| POST / GET | `/queue/jobs`, `/queue` | Submit material/target/priority and read the global queue plus reported device state. |
| GET / PATCH | `/materials`, `/materials/{material_id}` | Read fixed hardware mappings and safely rename material labels. |
| POST | `/queue/jobs/{command_id}/cancel` | Cancel a waiting server/local job. |
| POST | `/queue/control` | Enqueue CANCEL, PAUSE, RESUME, ESTOP or CLEAR for the ESP32. |
| GET / POST | `/device/commands`, `/device/commands/{id}/ack` | ESP32 command polling and acknowledgement. |
| POST / GET | `/device/status` | Receive and inspect channel, relay, scale and queue state. |
| GET / POST | `/profiles`, `/profiles/active`, `/profiles/{id}/{version}/activate` | Read active profiles, save immutable versions, and select a version for future jobs. |

The existing telemetry and run endpoints below remain the source for graph,
history, run detail and CSV export.

The API contract is frozen at `docs/telemetry/CONTRACT.md`; this README is the
deployment and operations guide for the same system, and every endpoint and
field below matches that document.

### Code layout: routes vs services

Business logic lives in transport-neutral modules under `app/services/` that
take `(db session, validated args)` and return dicts / pydantic models, or raise
`ServiceError(code, message, http_status=None)` (`services/errors.py`; no
fastapi import). `app/routes/*` are thin wrappers: auth dependencies, request
parsing and `routes/_adapt.py::http_errors()` (ServiceError -> HTTPException,
same status and detail as before). A future MQTT dispatcher calls the services
directly. Telemetry stays observability only: services queue device commands,
they never drive a relay.

| Module | Holds |
|---|---|
| `services/queue.py` | queue job create / list / cancel / HOLD-RELEASE-PROMOTE / resubmit, profile pin checks |
| `services/control.py` | control commands (incl. ZERO/TARE gating), HTTP command poll, command ACK, `command_wire`, device status, command transport |
| `services/runs.py` | targets, dashboard, run search / latest / get / samples / events / full / csv / delete, live |
| `services/profiles.py` | version rules plus profile list / active / create / bulk / next-version / activate / deactivate / delete |
| `services/materials.py` | material list and rename |
| `services/telemetry_ingest.py` | runs/start, telemetry batch, events, run complete |
| `services/command_claim.py`, `live_state.py` | unchanged (atomic HTTP/MQTT claim; in-memory live state) |

Service tests: `tests/test_services.py`.

---

## 1. Networking: Windows LAN IP vs WSL 172.x

The single most common deployment mistake: the ESP32 must target the **Windows
laptop's LAN IPv4 address** (e.g. `192.168.1.50`), **not** the WSL-internal
`172.x.x.x` address. The ESP32 lives on the physical LAN and has no route to
the WSL virtual switch; `172.x` is only reachable from inside WSL (or from
Windows via WSL's NAT). FastAPI binds `HOST=0.0.0.0`, but that alone does not
make it reachable from the LAN — the host-to-WSL path must be bridged.

### Option A (preferred): WSL2 mirrored networking

Mirrored mode makes WSL share the Windows host's network interfaces, so the
Windows LAN IP reaches the service directly — no portproxy, and no WSL IP to
track across restarts.

1. Create `%UserProfile%\.wslconfig` (e.g.
   `C:\Users\<you>\.wslconfig`) containing:

   ```ini
   [wsl2]
   networkingMode=mirrored
   ```

2. Shut down and restart WSL so the setting takes effect:

   ```bash
   wsl --shutdown
   ```

   Then start your Ubuntu distro again and re-run the server. In mirrored mode
   the WSL service is reachable at the **Windows LAN IP** on the bound port.

### Option B: port forwarding (admin PowerShell on Windows)

Without mirrored mode, forward a Windows port to the WSL service. Run this in
an **Administrator PowerShell on Windows** (not inside WSL). The repository
script detects the current WSL address and restricts inbound access to the
dispensing subnet:

```powershell
.\server\tools\configure-wsl-lan.ps1
```

Defaults target the current bench network (`192.168.137.1`, sender subnet
`192.168.137.0/24`, port 8000). If your Windows LAN IPv4 or ESP32 subnet is
different, pass `-ListenAddress` and `-RemoteAddress` from `ipconfig` and your
device network. Re-run after a WSL restart because its IP can change. To view
or remove the rule:

```powershell
netsh interface portproxy show v4tov4
netsh interface portproxy delete v4tov4 listenport=8000 listenaddress=192.168.137.1
```

Equivalent manual setup, if preferred:

```powershell
netsh interface portproxy add v4tov4 listenport=8000 listenaddress=0.0.0.0 connectport=8000 connectaddress=$(wsl hostname -I)
```

`hostname -I` (run inside WSL) returns the current WSL IP; if it changes after
a WSL restart, re-run the `netsh` command with the new address. To inspect or
remove the rule:

```powershell
netsh interface portproxy show v4tov4
netsh interface portproxy delete v4tov4 listenport=8000 listenaddress=0.0.0.0
```

### Windows Firewall inbound rule (admin PowerShell)

Required for either option so the LAN can reach port 8000:

```powershell
New-NetFirewallRule -DisplayName "Dispense Telemetry 8000" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow
```

### Finding the two addresses

- **Windows LAN IP:** `ipconfig` on Windows — use the *IPv4 Address* of the
  active adapter (Wi-Fi or Ethernet).
- **WSL IP:** `hostname -I` inside WSL (or `wsl hostname -I` from Windows).

### Verify from the LAN

From a phone or another PC on the same network, open:

```
http://<windows-lan-ip>:8000/health
```

or from a terminal:

```bash
curl http://<windows-lan-ip>:8000/health
# {"status":"ok","database":"up","last_telemetry_at":null,"run_count":0}
```

If `/health` answers but the ESP32 still cannot upload, re-check the IP the
firmware is configured with (`WEIGHT_DEMO_TELEMETRY_SERVER_IP`) — it must be
the Windows LAN IP from `ipconfig`, never a `172.x` address.

---

## 2. Install and run (WSL / Ubuntu)

All commands below run **inside the Ubuntu WSL shell**, from the repository
root, unless noted.

### 2.1 MySQL

```bash
sudo apt update && sudo apt install -y mysql-server
```

Ensure the daemon is running (start it now and enable it at boot):

```bash
sudo service mysql start
sudo systemctl enable mysql
```

Create the database and application user. Replace `CHANGE_ME` with a real
password and use the same value for `DB_PASSWORD` in `.env`:

```bash
sudo mysql -e "CREATE DATABASE dispense_telemetry CHARACTER SET utf8mb4; CREATE USER 'dispense'@'%' IDENTIFIED BY 'CHANGE_ME'; GRANT ALL PRIVILEGES ON dispense_telemetry.* TO 'dispense'@'%'; FLUSH PRIVILEGES;"
```

Note: on a default Ubuntu install the MySQL `root` account uses
`auth_socket`, so `sudo mysql` (not `mysql -u root -p`) is required for these
administrative statements.

### 2.2 Python environment and dependencies

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 2.3 Configuration

```bash
cp .env.example .env
```

Then edit `.env` and set at least `DB_PASSWORD`, `API_KEY` and, if MySQL is not
local to the same host, `DB_HOST`. `.env` is git-ignored — never commit real
credentials. See section 3 for every variable.

### 2.4 Run

```bash
uvicorn app.main:app --host 0.0.0.0 --port 8000 --workers 1 --ws-max-size 1024
```

Binding `0.0.0.0` is required so the ESP32 and LAN browsers can reach the
service. On startup the application runs `create_all` and additive schema
migrations for the dual-channel fields and query indexes. Existing run/sample
rows are preserved; startup does not delete or rewrite telemetry, and there
are no purge or cascade-delete jobs.

---

## 3. Configuration reference

Every setting comes from the environment; `.env` is loaded for convenience.
Source of truth: `server/.env.example` and `server/app/config.py`.

| Variable | Default | Meaning |
|---|---|---|
| `DB_HOST` | `127.0.0.1` | MySQL host used to build the SQLAlchemy URL. |
| `DB_PORT` | `3306` | MySQL port. |
| `DB_USER` | `dispense` | MySQL user. |
| `DB_PASSWORD` | *(empty)* | MySQL password for `DB_USER`. |
| `DB_NAME` | `dispense_telemetry` | MySQL database name. |
| `DATABASE_URL` | *(unset)* | Full SQLAlchemy URL override; **wins over all `DB_*` values** when set. Production form: `mysql+pymysql://user:pass@host:3306/dbname?charset=utf8mb4`. Tests may use `sqlite:///./test.db` or `sqlite://`. |
| `HOST` | `0.0.0.0` | Uvicorn bind address. Keep `0.0.0.0` so the ESP32 and LAN browsers can reach it. |
| `PORT` | `8000` | Uvicorn port; must match the firmware's `WEIGHT_DEMO_TELEMETRY_SERVER_PORT`. |
| `API_KEY` | *(empty)* | Shared key checked against the `X-API-Key` header on every write. Empty = writes open (bench mode). Must match `WEIGHT_DEMO_TELEMETRY_API_KEY` in the firmware config. |
| `READS_REQUIRE_KEY` | `false` | When `true`, dashboard read endpoints and the live WebSocket also require `X-API-Key` (WebSocket: header or first `auth` frame). |
| `CORS_ORIGINS` | *(empty)* | Comma-separated allowed origins. Empty = same-origin only, which is the normal case: the dashboard is served by this server and the ESP32 sends no CORS preflight for its simple JSON POSTs. Also the WebSocket `Origin` allowlist beyond same-host. |
| `LIVE_WS_DEVICE_WHITELIST` | *(empty)* | Comma-separated sender ids that may receive ZERO/TARE over the live WebSocket. Empty refuses every command. |
| `LIVE_WS_AUTH_TIMEOUT_MS` | `3000` | With `READS_REQUIRE_KEY=true`, time a WebSocket client has to send its `auth` frame before a 1008 close (200..30000). |
| `LIVE_WS_CLIENT_TIMEOUT_MS` | `3000` | Time to wait for a device ack before reporting `unknown` to the WebSocket client (500..30000). |
| `LIVE_WS_SEND_TIMEOUT_MS` | `2000` | Per-frame send timeout; a stalled client is disconnected (200..30000). |
| `MAX_WS_CLIENTS` | `16` | Maximum concurrent live WebSocket clients, pre-auth sockets included; extra handshakes get 1013 (1..256). |
| `BENCH_OPEN_WRITES` | `false` | Bench only: with an empty `API_KEY`, allow ZERO/TARE from loopback WebSocket clients without a key. Never on a LAN deployment. |

---

## 4. curl cookbook

The examples use run_id `bits-a4cf12ab34cd-j7-1834-a3f9`, device
`bits-a4cf12ab34cd`, job 7 and target 5000 g. Set the base URL and key once:

```bash
BASE=http://localhost:8000
API_KEY=change-me        # must equal the server's API_KEY
```

On **Windows PowerShell**, `curl` is an alias for `Invoke-WebRequest`; use
`curl.exe` and single-quoted JSON, or run these from **Git Bash** where the
commands below work verbatim.

### Health (read, no key)

```bash
curl $BASE/health
# {"status":"ok","database":"up","last_telemetry_at":"2026-10-02T14:03:22.512Z","run_count":3}
```

### Start a run (write, key required)

Registers the run and assigns a server-side `test_number`. Safe to retry: an
existing `run_id` returns the same `200` body instead of erroring.

```bash
curl -X POST $BASE/api/v1/runs/start \
  -H "Content-Type: application/json" \
  -H "X-API-Key: $API_KEY" \
  -d '{
    "run_id": "bits-a4cf12ab34cd-j7-1834-a3f9",
    "device_id": "bits-a4cf12ab34cd",
    "job_id": 7,
    "target_g": 5000,
    "priority": 0,
    "firmware": "relay-controller 6.1-telemetry",
    "config": {
      "kp": 0.0025, "ki": 0.0003, "kd": 0.0001, "integral_max": 200.0,
      "tolerance_g": 20, "coarse_transition_g": 1000, "settle_ms": 1500,
      "max_duration_ms": 120000, "max_overshoot_g": 100, "window_ms": 500,
      "min_on_ms": 40, "min_off_ms": 40, "correction_limit": 5,
      "completion_mode": "PROCESS"
    }
  }'
# {"run_id":"bits-a4cf12ab34cd-j7-1834-a3f9","test_number":12,"status":"RUNNING","started_at":"..."}
```

### Upload a sample batch (write, key required)

Between 1 and 500 samples per batch. Retried batches are idempotent — rows
whose `(run_id, idx)` is already stored are silently skipped. Unknown `run_id`
returns `409`.

```bash
curl -X POST $BASE/api/v1/telemetry/batch \
  -H "Content-Type: application/json" \
  -H "X-API-Key: $API_KEY" \
  -d '{
    "run_id": "bits-a4cf12ab34cd-j7-1834-a3f9",
    "device_id": "bits-a4cf12ab34cd",
    "samples": [
      {"idx": 1, "uptime_ms": 1834567, "elapsed_ms": 1200, "seq": 45,
       "weight_g": 4820, "target_g": 5000, "error_g": 180,
       "stable": false, "weight_age_ms": 120,
       "p_term": 0.45, "i_term": 0.02, "d_term": 0.0, "pid_output": 0.47,
       "relay1": true, "relay2": false, "state": "COARSE_DISPENSE"},
      {"idx": 2, "uptime_ms": 1835567, "elapsed_ms": 2200, "seq": 46,
       "weight_g": 4992, "target_g": 5000, "error_g": 8,
       "stable": false, "weight_age_ms": 110,
       "p_term": 0.02, "i_term": 0.03, "d_term": 0.0, "pid_output": 0.05,
       "relay1": false, "relay2": true, "state": "FINE_DISPENSE"}
    ]
  }'
# {"run_id":"bits-a4cf12ab34cd-j7-1834-a3f9","accepted":2,"inserted":2,"duplicates":0}
```

### Upload events (write, key required)

Between 1 and 100 events per call; same `(run_id, idx)` idempotency. Event
names are the enum in CONTRACT.md §2 (e.g. `COARSE_STARTED`, `TARGET_REACHED`,
`JOB_COMPLETE`, `JOB_FAILED`, `OVERWEIGHT`, `WEIGHT_LINK_LOST`).

```bash
curl -X POST $BASE/api/v1/events \
  -H "Content-Type: application/json" \
  -H "X-API-Key: $API_KEY" \
  -d '{
    "run_id": "bits-a4cf12ab34cd-j7-1834-a3f9",
    "device_id": "bits-a4cf12ab34cd",
    "events": [
      {"idx": 1, "event": "COARSE_STARTED", "elapsed_ms": 900,
       "state": "COARSE_DISPENSE", "weight_g": 120, "detail": null},
      {"idx": 2, "event": "TARGET_REACHED", "elapsed_ms": 44800,
       "state": "SETTLING", "weight_g": 5002, "detail": null}
    ]
  }'
# {"run_id":"bits-a4cf12ab34cd-j7-1834-a3f9","accepted":2,"inserted":2,"duplicates":0}
```

### Complete a run (write, key required)

`status` is one of `COMPLETE`, `FAILED`, `CANCELLED`. Returns the full run
object. Re-completing a terminal run is idempotent (returns the stored run).
Unknown `run_id` returns `404`.

```bash
curl -X POST $BASE/api/v1/runs/bits-a4cf12ab34cd-j7-1834-a3f9/complete \
  -H "Content-Type: application/json" \
  -H "X-API-Key: $API_KEY" \
  -d '{
    "status": "COMPLETE",
    "final_weight_g": 5002,
    "max_weight_g": 5040,
    "overshoot_g": 40,
    "duration_ms": 45210,
    "corrections": 1,
    "error": null
  }'
# 200: the full run object (run_id, status, final_weight_g, final_error_g, ...)
```

### Reads (open unless READS_REQUIRE_KEY=true)

```bash
# Per-target aggregation for the dashboard: targets ordered 5000/10000/15000/20000
# first, then remaining ascending; each with a summary and up to 10 newest run cards.
curl "$BASE/api/v1/dashboard"

# History search: filter by target/status/from/to/run_id with paging.
# Returns {"items":[<run card>...],"total":n,"limit":50,"offset":0}.
curl "$BASE/api/v1/runs?target_g=5000&limit=50"

# Newest runs for one target (or all targets if target_g is omitted).
curl "$BASE/api/v1/runs/latest?target_g=5000&limit=10"

# Live view: newest RUNNING run (or newest terminal run with is_live:false),
# its last <=300 samples, and the latest sample.
curl "$BASE/api/v1/live"
```

### Download a run as CSV

```bash
curl -OJ "$BASE/api/v1/runs/bits-a4cf12ab34cd-j7-1834-a3f9/csv"
```

`-J` honours the server's suggested filename; `-O` writes it to disk. The file
starts with `#`-comment run header lines, followed by sample rows with columns
`idx,timestamp,elapsed_ms,uptime_ms,seq,weight_g,target_g,error_g,stable,weight_age_ms,p_term,i_term,d_term,pid_output,relay1,relay2,state`.

---

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| ESP32 cannot reach the server | Firmware pointed at the WSL `172.x` address, which is not routable from the LAN | Point `WEIGHT_DEMO_TELEMETRY_SERVER_IP` at the **Windows LAN IPv4** (`ipconfig`); enable WSL mirrored networking (section 1, Option A) or add the portproxy rule (Option B). |
| ESP32 cannot reach the server (IP is correct) | Windows Firewall blocking inbound 8000, or portproxy missing/pointing at a stale WSL IP | Add the `New-NetFirewallRule` inbound rule; re-run `netsh ... connectaddress=$(wsl hostname -I)` after each WSL restart. |
| `/health` shows `"database":"down"` | MySQL not running, wrong credentials, or the database was never created | `sudo service mysql start`; check `DB_*` in `.env`; create the DB/user with the SQL in section 2.1. |
| `401` on any write | Server `API_KEY` and firmware `WEIGHT_DEMO_TELEMETRY_API_KEY` differ (or the header is missing) | Make both values identical; send `-H "X-API-Key: $API_KEY"`. If you intend open writes, clear `API_KEY` on the server. |
| `409 {"detail":"unknown run_id: ..."}` on batch/events | The device uploaded samples before its `/runs/start` was acknowledged | Expected and safe: the firmware buffers and retries. Ensure `POST /runs/start` succeeds first; samples/events for a run are only uploaded after its start is acknowledged. No data is lost. |
| `422` validation error | Payload violates contract limits — e.g. more than 500 samples or 100 events, `idx < 1`, `elapsed_ms < 0`, bad `run_id` pattern, or an event name outside the enum | Fix the payload per CONTRACT.md §3. Large batches must be split to the 500/100 limits. |
| `503 {"detail":"database unavailable: ..."}` | MySQL went down mid-request | Restart MySQL and retry. The firmware retries; buffered samples are re-sent. |
| ESP32 stalls while the server is down | Should not happen | Telemetry never blocks control: uploads have a short timeout and happen only in the telemetry task. If dispensing is affected, the problem is in the control path, not this server. |

---

## 6. Schema + API summary

MySQL, SQLAlchemy-managed, auto-created at startup. All timestamps are UTC
`DATETIME(3)`. Nothing is ever deleted or overwritten by the application.

### Tables

**dispense_runs** (one row per physical dispensing attempt)

| Column | Type | Notes |
|---|---|---|
| `run_id` | VARCHAR(64) PK | Immutable, client-generated by the ESP32. |
| `device_id` | VARCHAR(64) | Indexed. |
| `job_id` | INT | Local ESP32 job id. |
| `test_number` | INT | Per-target ordinal (1-based), assigned by the server at start; display-only. |
| `target_g` | INT | Indexed. |
| `priority` | TINYINT | 0 normal, 1 high. |
| `status` | VARCHAR(16) | RUNNING / COMPLETE / FAILED / CANCELLED. |
| `started_at` / `completed_at` | DATETIME(3) UTC | `started_at` indexed; `completed_at` nullable. |
| `duration_ms` | INT | Nullable. |
| `kp`, `ki`, `kd`, `integral_max` | FLOAT | PID gains used. |
| `tolerance_g`, `coarse_transition_g`, `max_overshoot_g`, `settle_ms`, `max_duration_ms`, `window_ms`, `min_on_ms`, `min_off_ms`, `correction_limit` | INT | Config snapshot. |
| `completion_mode` | VARCHAR(32) | PROCESS / RAMP_TEST. |
| `start_weight_g`, `final_weight_g`, `final_error_g`, `overshoot_g`, `max_weight_g` | INT | Results; `final_error_g` and `overshoot_g` computed by the server when absent. |
| `corrections`, `error_text`, `firmware` | INT / VARCHAR(120) / VARCHAR(64) | Correction count, failure reason, firmware string. |
| `sample_count`, `event_count` | INT | Maintained by the server. |
| `created_at` | DATETIME(3) UTC | Server receipt time. |

**weight_samples** (append-only per-run sample stream)

| Column | Type | Notes |
|---|---|---|
| `id` | BIGINT PK auto | |
| `run_id` | VARCHAR(64) FK -> dispense_runs | Indexed; `UNIQUE(run_id, idx)`. |
| `idx` | INT | Per-run sample counter from the ESP32 — the idempotency key. |
| `timestamp` | DATETIME(3) UTC | Indexed; server-derived as `started_at + elapsed_ms`. |
| `elapsed_ms`, `uptime_ms`, `seq` | INT / BIGINT / INT | Timing; `seq` nullable. |
| `weight_g`, `target_g`, `error_g` | INT | `error_g = target - weight` (positive = need more). |
| `stable`, `weight_age_ms` | BOOL / INT | Scale stable flag; weight-packet age. |
| `p_term`, `i_term`, `d_term`, `pid_output` | FLOAT | `pid_output` = fine duty [0..1]. |
| `relay1`, `relay2`, `state` | BOOL / BOOL / VARCHAR(24) | Coarse / fine valve states; controller state name. |

**dispense_events** (append-only lifecycle log)

| Column | Type | Notes |
|---|---|---|
| `id` | BIGINT PK auto | |
| `run_id` | VARCHAR(64) FK | Indexed; `UNIQUE(run_id, idx)`. |
| `idx` | INT | Per-run event counter — the idempotency key. |
| `timestamp` | DATETIME(3) UTC | Indexed. |
| `elapsed_ms` | INT | Nullable. |
| `event` | VARCHAR(32) | Enum: `JOB_CREATED`, `JOB_STARTED`, `COARSE_STARTED`, `COARSE_STOPPED`, `FINE_STARTED`, `TARGET_REACHED`, `SETTLING_STARTED`, `FINE_CORRECTION`, `JOB_COMPLETE`, `JOB_FAILED`, `JOB_CANCELLED`, `OVERWEIGHT`, `WEIGHT_LINK_LOST`, `PAUSE`, `RESUME`, `CANCEL`, `EMERGENCY_STOP`, `STATE_CHANGE`. |
| `state`, `weight_g`, `detail` | VARCHAR(24) / INT / VARCHAR(240) | Nullable context. |

**server_meta** (tiny key/value table)

| Column | Type | Notes |
|---|---|---|
| `key` | VARCHAR(64) PK | e.g. `last_telemetry_at`. |
| `value` | VARCHAR(190) | Value string. |

### Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/health` | none | Server + database status, last telemetry time, run count. |
| POST | `/api/v1/runs/start` | write key | Register a run; idempotent. Assigns `test_number`. |
| POST | `/api/v1/telemetry/batch` | write key | Ingest 1–500 weight samples; idempotent by `(run_id, idx)`. |
| POST | `/api/v1/events` | write key | Ingest 1–100 lifecycle events; idempotent by `(run_id, idx)`. |
| POST | `/api/v1/runs/{run_id}/complete` | write key | Mark a run terminal; idempotent; returns the full run object. |
| GET | `/api/v1/targets` | read (key only if `READS_REQUIRE_KEY`) | Distinct targets with run counts, ascending. |
| GET | `/api/v1/dashboard` | read (key only if `READS_REQUIRE_KEY`) | Per-target summary + up to 10 newest run cards. |
| GET | `/api/v1/runs/latest` | read (key only if `READS_REQUIRE_KEY`) | Newest runs, optional `target_g` and `limit`. |
| GET | `/api/v1/runs` | read (key only if `READS_REQUIRE_KEY`) | History search by target/from/to/status/run_id, paged. |
| GET | `/api/v1/runs/{run_id}` | read (key only if `READS_REQUIRE_KEY`) | Full run object + `final_error_pct`. |
| GET | `/api/v1/runs/{run_id}/samples` | read (key only if `READS_REQUIRE_KEY`) | Samples; optional `max_points` decimation (display only). |
| GET | `/api/v1/runs/{run_id}/events` | read (key only if `READS_REQUIRE_KEY`) | Events, ascending. |
| GET | `/api/v1/runs/{run_id}/full` | read (key only if `READS_REQUIRE_KEY`) | Run + samples + events + stats in one call. |
| GET | `/api/v1/runs/{run_id}/csv` | read (key only if `READS_REQUIRE_KEY`) | CSV download of the run's samples. |
| GET | `/api/v1/live` | read (key only if `READS_REQUIRE_KEY`) | Newest active run, last ≤300 samples, latest sample. |

Auth column: "write key" means `X-API-Key` is required whenever the server's
`API_KEY` is non-empty (empty = writes open). "read (key only if
`READS_REQUIRE_KEY`)" means read endpoints are open unless
`READS_REQUIRE_KEY=true`. Missing or invalid key -> `401`; validation -> `422`;
unknown run on batch/events -> `409`; unknown run on complete -> `404`;
database down -> `503`.

**Authoritative reference: `docs/telemetry/CONTRACT.md`.**
