"""Static checks of deploy/mosquitto templates and gen_config.py (REQ-WMQ-29, HZ-10). No broker needed."""
import base64
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
DEPLOY = ROOT / "deploy" / "mosquitto"
INV = {"senders": ["snd1", "snd2"],
       "relays": [{"id": "rly1", "sender": "snd1"}, {"id": "rly2", "sender": "snd2"}],
       "stations": ["st1"]}


@pytest.fixture(scope="module")
def gen():
    sys.path.insert(0, str(DEPLOY))
    import gen_config
    return gen_config


def blocks(text):
    out, user = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("user "):
            user = line.split()[1]
            out[user] = []
        elif user and line.startswith("topic "):
            out[user].append(tuple(line.split()[1:]))
    return out


def test_templates_are_ascii_and_free_of_secrets():
    for name in ("bits.conf", "acl.conf", "gen_config.py"):
        raw = (DEPLOY / name).read_bytes()
        assert all(b < 128 for b in raw), f"{name} contains non-ASCII bytes"
        assert not re.search(rb"\$[67]\$\d*\$?[A-Za-z0-9+/=]{12,}\$", raw), f"{name} holds a hash"
        assert not re.search(rb"^[A-Za-z0-9_-]+:\$[67]\$", raw, re.M), f"{name} holds a passwd line"
    assert not (DEPLOY / "passwd").exists() and not (DEPLOY / "mqtt-users.json").exists()


def test_bits_conf_hardening_lines():
    active = [l.strip() for l in (DEPLOY / "bits.conf").read_text().splitlines()
              if l.strip() and not l.lstrip().startswith("#")]
    assert "allow_anonymous false" in active
    assert "use_username_as_clientid true" in active
    assert [l for l in active if l.startswith("listener ")] == ["listener 1883"]
    assert any(l.startswith("password_file ") for l in active)
    assert any(l.startswith("acl_file ") for l in active)
    assert not any("8883" in l or "cafile" in l or "certfile" in l for l in active)


def test_render_expands_every_identity_and_no_default_rules(gen):
    text = gen.render((DEPLOY / "acl.conf").read_text(), INV)
    first = next(l for l in text.splitlines() if l.strip())
    assert first.startswith("user "), "rules before the first user line would grant default access"
    assert not any(l.startswith("pattern ") for l in text.splitlines())
    acl = blocks(text)
    assert set(acl) == {"bits_backend", "bits_admin", "dev_snd1", "dev_snd2", "dev_rly1", "dev_rly2",
                        "app_st1"}
    assert ("write", "cas/+/commands") in acl["bits_backend"]
    assert ("read", "cas/snd1/weight/ctl") in acl["dev_rly1"]
    assert ("read", "cas/snd2/weight/ctl") in acl["dev_rly2"]
    assert all(k == "read" for k, _t in acl["bits_admin"])
    assert "{" not in text and "}" not in text


def test_render_only_backend_writes_commands(gen):
    acl = blocks(gen.render((DEPLOY / "acl.conf").read_text(), INV))
    for user, rules in acl.items():
        for kind, topic in rules:
            if kind == "write" and topic.endswith("/commands"):
                assert user == "bits_backend", f"{user} may publish commands via {topic}"


def test_render_rejects_rules_before_first_user(gen):
    with pytest.raises(ValueError):
        gen.render("topic read cas/#\nuser bits_backend\ntopic read cas/+/status\n", INV)


@pytest.mark.parametrize("bad", ["", "a/b", "a+b", "a#", "a b", "x" * 33, "snd1\n", "../x"])
def test_invalid_identity_is_refused(gen, bad):
    with pytest.raises(ValueError):
        gen.validate_inventory({"senders": [bad], "relays": [], "stations": []})


def test_relay_peer_must_be_a_listed_sender(gen):
    with pytest.raises(ValueError):
        gen.validate_inventory({"senders": ["snd1"], "relays": [{"id": "rly1", "sender": "snd9"}],
                                "stations": []})


def test_hash_password_is_mosquitto_pbkdf2_sha512(gen):
    h = gen.hash_password("correct-horse-1")
    m = re.fullmatch(r"\$7\$(\d+)\$([A-Za-z0-9+/=]+)\$([A-Za-z0-9+/=]+)", h)
    assert m, h
    iterations, salt, digest = int(m.group(1)), base64.b64decode(m.group(2)), base64.b64decode(m.group(3))
    assert iterations >= 101 and len(salt) >= 12 and len(digest) == 64
    assert hashlib.pbkdf2_hmac("sha512", b"correct-horse-1", salt, iterations, 64) == digest
    assert gen.hash_password("correct-horse-1") != h


def test_cli_writes_outputs_and_never_prints_secrets(gen, tmp_path):
    inv, out, sec = tmp_path / "inv.json", tmp_path / "out", tmp_path / "secrets.json"
    inv.write_text(json.dumps(INV))
    r = subprocess.run([sys.executable, str(DEPLOY / "gen_config.py"), "--inventory", str(inv),
                        "--out-dir", str(out), "--secrets", str(sec)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    passwords = json.loads(sec.read_text())
    assert set(passwords) == {"bits_backend", "bits_admin", "dev_snd1", "dev_snd2", "dev_rly1",
                              "dev_rly2", "app_st1"}
    assert all(len(p) >= 20 for p in passwords.values())
    for p in passwords.values():
        assert p not in r.stdout and p not in r.stderr
    passwd = (out / "passwd").read_text().splitlines()
    assert sorted(l.split(":")[0] for l in passwd) == sorted(passwords)
    assert all(l.split(":", 1)[1].startswith("$7$") for l in passwd)
    assert "user dev_snd1" in (out / "acl.conf").read_text()
    again = subprocess.run([sys.executable, str(DEPLOY / "gen_config.py"), "--inventory", str(inv),
                            "--out-dir", str(out), "--secrets", str(sec)], capture_output=True, text=True)
    assert again.returncode == 0 and json.loads(sec.read_text()) == passwords


def test_cli_refuses_bad_inventory(tmp_path):
    inv = tmp_path / "inv.json"
    inv.write_text(json.dumps({"senders": ["bad/id"], "relays": [], "stations": []}))
    r = subprocess.run([sys.executable, str(DEPLOY / "gen_config.py"), "--inventory", str(inv),
                        "--out-dir", str(tmp_path / "out"), "--secrets", str(tmp_path / "s.json")],
                       capture_output=True, text=True)
    assert r.returncode != 0 and not (tmp_path / "s.json").exists()
