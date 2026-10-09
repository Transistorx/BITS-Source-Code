# BITS Source Code

- `esp32-relay-controller/` - ESP32 relay controller (ESP-IDF, C): dual-channel dispensing control, safety manager, job queue, telemetry.
- `esp32-weight-sender/` - ESP32 weight sender (ESP-IDF, C): reads a CAS scale over RS232 and forwards weight data.
- `server/` - FastAPI + SQLAlchemy telemetry/operator server with Jinja and vanilla JS frontend, pytest tests.

Status: snapshot, work in progress. Firmware changes are NOT hardware-validated. The relay controller control logic is under safety audit and must not be flashed to dispensing hardware without review. No secrets are included.

Build firmware with ESP-IDF v6.1 (`idf.py build` in each firmware folder). The server's target architecture is MQTT-only; that migration is in progress.
