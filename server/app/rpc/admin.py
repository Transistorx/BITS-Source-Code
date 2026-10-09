"""Operator administration CLI.

    python -m app.rpc.admin create-operator --username alice --role operator

The password is read from the BITS_OPERATOR_PASSWORD environment variable or an
interactive prompt (twice). It is never accepted as an argument and never logged.
"""

import argparse
import getpass
import os
import sys

from ..services import auth as auth_service
from ..services.errors import ServiceError


def create_operator(session, username: str, role: str, password: str) -> int:
    row = auth_service.create_operator(session, username, password, role)
    return row.id


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m app.rpc.admin")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("create-operator", help="create an operator account")
    p.add_argument("--username", required=True)
    p.add_argument("--role", choices=sorted(auth_service.ROLES), default="operator")
    args = parser.parse_args(argv)

    password = os.environ.get("BITS_OPERATOR_PASSWORD")
    if not password:
        password = getpass.getpass("Password: ")
        if getpass.getpass("Repeat password: ") != password:
            print("passwords do not match", file=sys.stderr)
            return 2
    from .. import database
    database.init_db()
    with database._SessionLocal() as db:
        try:
            create_operator(db, args.username, args.role, password)
        except ServiceError as exc:
            print(f"error: {exc.message}", file=sys.stderr)
            return 1
    print(f"created {args.role} {args.username}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
