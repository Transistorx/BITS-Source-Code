"""Show the profile-versioning migration status (READ-ONLY by default).

    python scripts/migration_status.py [--json] [--audit N]
    python scripts/migration_status.py --retry --note "why" [--i-know]

Default mode only reads: it never creates tables, never runs init_db and never
migrates. It prints the resolved target (no password), the status derived from the
real schema + the recorded state, and the tail of schema_migration_audit.

--retry clears a FAILED marker so the NEXT server start may resume the migration.
It changes no table; it only writes the state record and one audit row. It is
refused unless the status is MIGRATION_FAILED and resumable, and unless the database
name ends in '_dryrun' or --i-know is given. Read the runbook first
(docs/cas-audit/12-profile-versioning-migration.md, section Partial failure).
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _target  # noqa: E402


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--audit", type=int, default=30, help="audit rows to show (default 30)")
    parser.add_argument("--retry", action="store_true",
                        help="clear the FAILED marker so the next start can resume (guarded)")
    parser.add_argument("--note", default="", help="free text stored in the audit row for --retry")
    parser.add_argument("--i-know", action="store_true",
                        help="allow --retry on a database name not ending in _dryrun (dangerous)")
    args = parser.parse_args(argv)

    url, line = _target.resolve()
    print(line)
    from app import database, migration_state as ms

    if args.retry and not _target.allowed(url, args.i_know):
        print(_target.REFUSAL)
        return 2
    engine = database.get_engine()
    try:
        if args.retry:
            try:
                status = ms.approve_retry(engine, args.note[:200])
            except ValueError as exc:
                print(f"RETRY REFUSED: {exc}")
                return 3
            print("retry approved: the next server start may resume the migration")
        else:
            status = ms.migration_status(engine)
        tail = ms.audit_tail(engine, max(0, args.audit))
    finally:
        engine.dispose()
    if args.json:
        print(json.dumps({"status": status, "audit": tail}, indent=2, default=str))
        return 0 if args.retry or status["state"] == ms.HEALTHY else 1
    print(f"state={status['state']} reason={status['reason']} resumable={status['resumable']} "
          f"schema_class={status['schema_class']}")
    print(f"recorded={status['recorded']}")
    print(f"schema={status['schema']}")
    for row in tail:
        print(f"  audit #{row['id']} {row['ts']} {row['step']:<28} {row['outcome']:<5} {row['detail']}")
    return 0 if args.retry or status["state"] == ms.HEALTHY else 1


if __name__ == "__main__":
    raise SystemExit(main())
