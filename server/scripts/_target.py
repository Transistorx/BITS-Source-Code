"""Shared target description and guard for the migration helper scripts.

Never returns or prints the password. The target is whatever
``app.config.settings.database_url`` resolves to, which means DATABASE_URL (if set
in the environment or server/.env) WINS over DB_NAME/DB_HOST/... .
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def resolve():
    """(sqlalchemy URL object, description line without password)."""
    from sqlalchemy.engine import make_url

    from app.config import settings

    url = make_url(settings.database_url)
    name = url.database or ""
    line = (f"target: driver={url.drivername} host={url.host} port={url.port} "
            f"user={url.username} database={name!r} "
            f"DATABASE_URL_in_environment={'yes (overrides DB_NAME)' if os.getenv('DATABASE_URL', '').strip() else 'no'}")
    return url, line


def is_dryrun_name(name: str) -> bool:
    """Disposable rehearsal databases end in _dryrun (e.g. dispense_telemetry_dryrun)."""
    return (name or "").lower().endswith("_dryrun")


def allowed(url, i_know: bool) -> bool:
    return bool(i_know) or is_dryrun_name(url.database or "")


REFUSAL = ("REFUSED: the resolved database name does not end in '_dryrun'. Point DB_NAME at "
           "the disposable rehearsal schema and make sure DATABASE_URL is unset in this shell. "
           "--i-know exists for a database you OWN and intend to modify; NEVER use it against "
           "production.")
