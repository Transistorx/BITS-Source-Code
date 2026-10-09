"""Run ONLY the startup schema migration (init_db) against a DISPOSABLE database.

Usage (from server/, see docs/cas-audit/12-profile-versioning-migration.md):

    unset DATABASE_URL
    DB_NAME=dispense_telemetry_dryrun python scripts/dryrun_profile_migration.py

Refuses to run unless the resolved database name ends in '_dryrun', or --i-know
is given (NEVER against production). DATABASE_URL, when set, overrides DB_NAME
(app/config.py), so the script prints the resolved target first: confirm the
'database=' value, and unset DATABASE_URL in the shell. It never starts the web app
or the MQTT bridge (the bridge uses the fixed client_id 'dispense-server'; a second
connection would kick the live bridge off the broker) and never prints the password.
"""

import argparse
import logging
import os
import sys
from pathlib import Path

os.environ["MQTT_ENABLED"] = "false"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import _target  # noqa: E402


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--i-know", action="store_true",
                        help="allow a target database name not ending in _dryrun (dangerous)")
    args = parser.parse_args(argv)

    url, line = _target.resolve()
    print(line)
    if not _target.allowed(url, args.i_know):
        print(_target.REFUSAL)
        return 2

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    from app import database

    # init_db() is exactly what server startup runs (additive column fixes, then
    # the status-gated migrate_profile_versioning, then the two Material rows).
    try:
        database.init_db()
    except Exception as exc:
        print(f"MIGRATION FAILED/REFUSED: {exc.__class__.__name__}: {exc}")
        print("inspect with: python scripts/migration_status.py")
        return 1
    print("migration completed; now run: python scripts/verify_profile_migration.py verify "
          "--baseline <snapshot.json>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
