"""Profile pin hash and check (CONTRACT 9.3). Pure helpers; no transport, no FastAPI.

profile_hash = first 16 hex of SHA-256 over the canonical compact JSON of the
``profile{}`` object exactly as ``command_wire`` sends it: keys sorted, separators
``,`` and ``:``, ASCII-escaped, floats in Python's shortest round-trip form
(0.0025, not 2.5e-3), integers without a fraction.
"""
import hashlib
import json

from sqlalchemy.orm import Session

from ..models import DeviceCommand
from . import queue as queue_service


def canonical_profile(profile: dict) -> str:
    return json.dumps(profile, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False)


def profile_hash(profile: dict) -> str:
    return hashlib.sha256(canonical_profile(profile).encode("ascii")).hexdigest()[:16]


def expected_pin(db: Session, job: DeviceCommand) -> dict | None:
    """{profile_id, version, hash} of what the server sent for this JOB, recomputed
    from the immutable pinned row; None if the pin cannot be resolved (fail closed)."""
    gains = queue_service.profile_gains(db, job)
    if gains is None:
        return None
    return {"profile_id": gains["profile_id"], "version": gains["version"],
            "hash": profile_hash(gains)}


def matches(db: Session, job: DeviceCommand, applied) -> bool:
    """True only if the device's applied_profile equals the job pin exactly."""
    if not isinstance(applied, dict):
        return False
    want = expected_pin(db, job)
    if want is None:
        return False
    got_hash = applied.get("hash")
    return (applied.get("profile_id") == want["profile_id"]
            and type(applied.get("version")) is int and applied["version"] == want["version"]
            and isinstance(got_hash, str) and got_hash.lower() == want["hash"]
            and job.profile_id == want["profile_id"] and job.profile_version == want["version"])
