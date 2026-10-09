import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import sys
from pathlib import Path

ID_RE = re.compile(r"[A-Za-z0-9_-]{1,32}")
FIXED_USERS = ("bits_backend", "bits_admin")
PBKDF2_ITERATIONS = 101
SALT_BYTES = 12
DIGEST_BYTES = 64
PASSWORD_BYTES = 24
EXIT_BAD_INPUT = 2
EXIT_IO = 3


def _one(value, where: str) -> str:
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise ValueError(f"{where}: invalid identity {value!r}")
    return value


def _ids(values, where: str) -> list[str]:
    if not isinstance(values, list):
        raise ValueError(f"{where}: must be a list")
    return [_one(v, where) for v in values]


def validate_inventory(inv) -> dict:
    if not isinstance(inv, dict):
        raise ValueError("inventory must be a JSON object")
    senders = _ids(inv.get("senders", []), "senders")
    stations = _ids(inv.get("stations", []), "stations")
    raw_relays = inv.get("relays", [])
    if not isinstance(raw_relays, list):
        raise ValueError("relays: must be a list")
    relays = []
    for item in raw_relays:
        if not isinstance(item, dict):
            raise ValueError("relays: each entry must be an object with id and sender")
        rid = _one(item.get("id"), "relays.id")
        peer = _one(item.get("sender"), "relays.sender")
        if peer not in senders:
            raise ValueError(f"relay {rid}: peer {peer} is not a listed sender")
        relays.append({"id": rid, "sender": peer})
    all_ids = senders + stations + [r["id"] for r in relays]
    if len(set(all_ids)) != len(all_ids):
        raise ValueError("duplicate identity in inventory")
    return {"senders": senders, "relays": relays, "stations": stations}


def users(inv) -> list[str]:
    inv = validate_inventory(inv)
    return [*FIXED_USERS,
            *(f"dev_{s}" for s in inv["senders"]),
            *(f"dev_{r['id']}" for r in inv["relays"]),
            *(f"app_{t}" for t in inv["stations"])]


def _fill(block: list[str], values: dict) -> str:
    lines = []
    for line in block:
        for key, val in values.items():
            line = line.replace("{" + key + "}", val)
        lines.append(line)
    return "\n".join(lines)


def render(template: str, inv) -> str:
    inv = validate_inventory(inv)
    blocks: list[list[str]] = []
    current: list[str] | None = None
    for raw in template.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("user "):
            current = [line]
            blocks.append(current)
            continue
        if current is None:
            raise ValueError("acl template has rules before the first user line")
        if not line.startswith("topic "):
            raise ValueError(f"acl template: only user and topic lines are allowed, got {line!r}")
        current.append(line)
    out = []
    for block in blocks:
        head = block[0]
        if "{relay}" in head:
            out.extend(_fill(block, {"relay": r["id"], "sender": r["sender"]}) for r in inv["relays"])
        elif "{sender}" in head:
            out.extend(_fill(block, {"sender": s}) for s in inv["senders"])
        elif "{station}" in head:
            out.extend(_fill(block, {"station": t}) for t in inv["stations"])
        else:
            out.append(_fill(block, {}))
    text = "\n\n".join(out) + "\n"
    if "{" in text or "}" in text:
        raise ValueError("unresolved placeholder in acl template")
    return text


def hash_password(password: str) -> str:
    salt = os.urandom(SALT_BYTES)
    digest = hashlib.pbkdf2_hmac("sha512", password.encode("utf-8"), salt, PBKDF2_ITERATIONS,
                                 DIGEST_BYTES)
    return "$7$%d$%s$%s" % (PBKDF2_ITERATIONS, base64.b64encode(salt).decode("ascii"),
                             base64.b64encode(digest).decode("ascii"))


def _write_private(path: Path, text: str) -> None:
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", newline="\n") as f:
        f.write(text)


def _load_existing(path: Path) -> dict:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not all(isinstance(k, str) and isinstance(v, str)
                                             for k, v in data.items()):
        raise ValueError("secrets file is not an object of string passwords")
    return data


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Render mosquitto ACL and password files for BITS.")
    ap.add_argument("--inventory", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--secrets", required=True, type=Path)
    ap.add_argument("--template", type=Path, default=Path(__file__).resolve().with_name("acl.conf"))
    args = ap.parse_args(argv)
    try:
        inv = validate_inventory(json.loads(args.inventory.read_text(encoding="utf-8")))
        acl_text = render(args.template.read_text(encoding="utf-8"), inv)
        existing = _load_existing(args.secrets)
    except (OSError, ValueError) as exc:
        print(f"gen_config: {exc}", file=sys.stderr)
        return EXIT_BAD_INPUT
    names = users(inv)
    passwords = {u: existing.get(u) or secrets.token_urlsafe(PASSWORD_BYTES) for u in names}
    passwd_text = "".join(f"{u}:{hash_password(passwords[u])}\n" for u in names)
    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "acl.conf").write_text(acl_text, encoding="utf-8", newline="\n")
        _write_private(args.out_dir / "passwd", passwd_text)
        _write_private(args.secrets, json.dumps(passwords, indent=2, sort_keys=True) + "\n")
    except OSError as exc:
        print(f"gen_config: write failed: {exc}", file=sys.stderr)
        return EXIT_IO
    reused = sum(1 for u in names if u in existing)
    print(f"gen_config: {len(names)} users ({reused} reused, {len(names) - reused} new) -> "
          f"{args.out_dir / 'acl.conf'}, {args.out_dir / 'passwd'}; secrets in {args.secrets}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
