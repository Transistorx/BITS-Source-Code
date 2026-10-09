"""Environment-driven server configuration.

Every value comes from the environment (a .env file is loaded for
convenience). No credentials live in source. See .env.example.
"""

import os
import secrets

from sqlalchemy.engine import URL

from dotenv import load_dotenv

load_dotenv()


def _bool(name: str, default: bool = False) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


_CLIENT_ID_DEFAULT = f"dispense-server-{os.getpid()}-{secrets.token_hex(2)}"


class Settings:
    @property
    def transient_command_ttl_seconds(self) -> int:
        return max(1, int(os.getenv("TRANSIENT_COMMAND_TTL_SECONDS", "10")))

    @property
    def scale_cmd_ttl_ms(self) -> int:
        try:
            return max(1, int(os.getenv("SCALE_CMD_TTL_MS", "3000")))
        except ValueError:
            return 3000

    @property
    def command_ttl_ms(self) -> int:
        """Default MQTT ttl_ms for non-transient, non-STOP command types."""
        return max(1000, int(os.getenv("COMMAND_TTL_MS", "30000")))

    @property
    def database_url(self) -> str:
        override = os.getenv("DATABASE_URL", "").strip()
        if override:
            return override
        host = os.getenv("DB_HOST", "127.0.0.1")
        port = os.getenv("DB_PORT", "3306")
        user = os.getenv("DB_USER", "dispense")
        password = os.getenv("DB_PASSWORD", "")
        name = os.getenv("DB_NAME", "dispense_telemetry")
        return URL.create(
            "mysql+pymysql", username=user, password=password, host=host,
            port=int(port), database=name, query={"charset": "utf8mb4"},
        ).render_as_string(hide_password=False)

    # Largest job target per pump (grams). MIRRORS a firmware constant (the relay
    # refuses a target above its own per-pump maximum); keep the two equal. Default
    # 20000 g = the 20 kg bench maximum. CONTRACT 9.3.
    def max_target_g(self, material_id: str) -> int:
        name = "PUMP1_MAX_TARGET_G" if material_id == "M1" else "PUMP2_MAX_TARGET_G"
        try:
            return min(100_000, max(1, int(os.getenv(name, "20000"))))
        except ValueError:
            return 20_000

    # Keep emitting the legacy PROFILE / PROFILE_CLEAR device commands on
    # activate / deactivate (schema-1 canonical profiles only). Default ON until
    # the firmware path that reads them is retired. Ranged (schema 2) profiles never emit them.
    @property
    def legacy_profile_commands(self) -> bool:
        return _bool("LEGACY_PROFILE_COMMANDS", True)

    @property
    def host(self) -> str:
        return os.getenv("HOST", "0.0.0.0")

    @property
    def port(self) -> int:
        return int(os.getenv("PORT", "8000"))

    # Shared API key for telemetry writes. Empty string = auth disabled.
    @property
    def api_key(self) -> str:
        return os.getenv("API_KEY", "").strip()

    @property
    def reads_require_key(self) -> bool:
        return _bool("READS_REQUIRE_KEY", False)

    @property
    def cors_origins(self) -> list[str]:
        raw = os.getenv("CORS_ORIGINS", "").strip()
        if not raw:
            return []
        return [o.strip() for o in raw.split(",") if o.strip()]

    # MQTT bridge (docs/telemetry/CONTRACT.md §7). Off by default so tests/CI
    # and HTTP-only deployments never need a broker.
    @property
    def mqtt_enabled(self) -> bool:
        return _bool("MQTT_ENABLED", False)

    @property
    def mqtt_broker_host(self) -> str:
        return os.getenv("MQTT_BROKER_HOST", "127.0.0.1")

    @property
    def mqtt_broker_port(self) -> int:
        return int(os.getenv("MQTT_BROKER_PORT", "1883"))

    @property
    def mqtt_client_id(self) -> str:
        return os.getenv("MQTT_CLIENT_ID", "").strip() or _CLIENT_ID_DEFAULT

    @property
    def mqtt_tls(self) -> bool:
        return _bool("MQTT_TLS", False)

    @property
    def mqtt_tls_ca_file(self) -> str:
        return os.getenv("MQTT_TLS_CA_FILE", "").strip()

    @property
    def live_ws_device_whitelist(self) -> list[str]:
        return [d.strip() for d in os.getenv("LIVE_WS_DEVICE_WHITELIST", "").split(",")
                if d.strip()]

    @property
    def bench_open_writes(self) -> bool:
        return _bool("BENCH_OPEN_WRITES", False)

    # Optional broker auth. Empty username = anonymous. Never log the password.
    @property
    def mqtt_username(self) -> str:
        return os.getenv("MQTT_USERNAME", "").strip()

    @property
    def mqtt_password(self) -> str:
        return os.getenv("MQTT_PASSWORD", "")


settings = Settings()
