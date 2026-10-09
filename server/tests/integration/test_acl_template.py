"""Static check of the generated ACL against CONTRACT 9.7 (no broker needed)."""
import sys

import pytest

from .harness import DEPLOY, INV


def _blocks():
    sys.path.insert(0, str(DEPLOY))
    import gen_config
    text = gen_config.render((DEPLOY / "acl.conf").read_text(encoding="utf-8"), INV)
    out, user = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("user "):
            user = line.split()[1]
            out[user] = []
        elif user and line.startswith("topic "):
            out[user].append(tuple(line.split()[1:]))
    return out


def test_backend_cannot_touch_weight_ctl_and_apps_cannot_touch_cas():
    acl = _blocks()
    for kind, topic in acl["bits_backend"]:
        assert "weight" not in topic and not topic.startswith("cas/#")
    assert all(not t.startswith("cas/") for _k, t in acl["app_st1"])
    assert ("write", "cas/+/run/ack") in acl["bits_backend"] and ("read", "cas/+/run/#") in acl["bits_backend"]
    assert ("write", "cas/rly1/run/#") in acl["dev_rly1"] and ("read", "cas/rly1/run/ack") in acl["dev_rly1"]
    assert ("read", "cas/snd1/weight/ctl") in acl["dev_rly1"]
    assert not [1 for k, t in acl["dev_rly1"] if k == "write" and "weight" in t]


def test_sender_may_publish_its_command_ack():
    assert ("write", "cas/snd1/commands/ack") in _blocks()["dev_snd1"]


def test_sender_ack_grant_is_exactly_one_extra_topic_and_nobody_else_has_it():
    acl = _blocks()
    assert ("write", "cas/snd1/commands/ack") in acl["dev_snd1"]
    assert ("write", "cas/snd1/commands/ack") not in acl["dev_snd2"]
    assert ("write", "cas/snd2/commands/ack") not in acl["dev_snd1"]
    writes = {t for k, t in acl["dev_snd1"] if k == "write"}
    assert writes == {"cas/snd1/status", "cas/snd1/telemetry/#", "cas/snd1/weight/#",
                      "cas/snd1/commands/ack"}
    assert {t for k, t in acl["dev_snd1"] if k == "read"} == {"cas/snd1/commands"}
    assert not [1 for u in ("bits_backend", "dev_rly1", "app_st1") for k, t in acl[u]
                if k == "write" and t == "cas/snd1/commands/ack"]
