"""Shared API-key header authentication.

The ESP32 sends `X-API-Key: <key>` on every telemetry write when
WEIGHT_DEMO_TELEMETRY_API_KEY is set in firmware config; the server checks
the same value from its API_KEY env. Empty API_KEY on the server disables
the check (open LAN bench mode). Read routes only require the key when
READS_REQUIRE_KEY=true.
"""

import hmac

from fastapi import Header, HTTPException, status

from .config import settings


def key_equals(presented: str | None, expected: str) -> bool:
    if not isinstance(presented, str):
        return False
    return hmac.compare_digest(presented.encode("utf-8", "replace"),
                               expected.encode("utf-8", "replace"))


def _check(x_api_key: str | None) -> None:
    expected = settings.api_key
    if not expected:
        return
    if not key_equals(x_api_key, expected):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid or missing X-API-Key",
        )


async def require_write_key(x_api_key: str | None = Header(default=None)) -> None:
    """Dependency for telemetry write endpoints."""
    _check(x_api_key)


async def require_read_key(x_api_key: str | None = Header(default=None)) -> None:
    """Dependency for read endpoints; enforced only when configured."""
    if settings.reads_require_key:
        _check(x_api_key)

