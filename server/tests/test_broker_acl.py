"""Broker ACL tests against an ISOLATED mosquitto on port 18830 (run inside WSL).

Uses deploy/mosquitto/bits.conf + gen_config.py output. Never touches the real broker.
Denied publishes are silent in MQTT 3.1.1, so denial is proven by non-delivery to an
authorised subscriber. Skipped when wsl/mosquitto/paho are unavailable.
"""
import json
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

mqtt = pytest.importorskip("paho.mqtt.client")

ROOT = Path(__file__).resolve().parents[2]
DEPLOY = ROOT / "deploy" / "mosquitto"
PORT = 18830
DISTRO = "Ubuntu"
NEG = 0.5   # seconds to wait to prove non-delivery
POS = 4.0   # max seconds to wait for delivery

INV = {
    "senders": ["snd1", "snd2"],
    "relays": [{"id": "rly1", "sender": "snd1"}, {"id": "rly2", "sender": "snd2"}],
    "stations": ["st1", "st2"],
}
TEMPLATES = ("bits.conf", "acl.conf", "gen_config.py")


def wsl(*args, **kw):
    return subprocess.run(["wsl", "-d", DISTRO, "-e", *args], capture_output=True, text=True, timeout=30, **kw)


def require_templates():
    missing = [name for name in TEMPLATES if not (DEPLOY / name).is_file()]
    if missing:
        pytest.fail(f"broker templates missing from git: {missing} (REQ-WMQ-29, HZ-10)")


def test_deploy_templates_present():
    require_templates()


@pytest.fixture(scope="module")
def broker(tmp_path_factory):
    require_templates()
    if not shutil.which("wsl"):
        pytest.skip("wsl not available")
    try:
        r = wsl("sh", "-c", "test -x /usr/sbin/mosquitto")
    except Exception as e:  # noqa: BLE001
        pytest.skip(f"wsl unusable: {e}")
    if r.returncode != 0:
        pytest.skip(f"mosquitto not installed in WSL distro {DISTRO}")
    with socket.socket() as s:
        s.settimeout(0.3)
        if s.connect_ex(("127.0.0.1", PORT)) == 0:
            pytest.skip(f"port {PORT} already in use")

    tmp = tmp_path_factory.mktemp("broker")
    inv = tmp / "inv.json"
    inv.write_text(json.dumps(INV))
    out, sec = tmp / "out", tmp / "secrets.json"
    subprocess.run([sys.executable, str(DEPLOY / "gen_config.py"), "--inventory", str(inv),
                    "--out-dir", str(out), "--secrets", str(sec)], check=True, capture_output=True)
    passwords = json.loads(sec.read_text())

    wdir = wsl("mktemp", "-d", "/tmp/bits-acl.XXXXXX").stdout.strip()
    assert wdir.startswith("/tmp/bits-acl.")
    src = wsl("wslpath", "-u", out.as_posix()).stdout.strip()
    conf = []
    for line in (DEPLOY / "bits.conf").read_text().splitlines():
        for key, val in (("listener 1883", f"listener {PORT}"),
                         ("password_file ", f"password_file {wdir}/passwd"),
                         ("acl_file ", f"acl_file {wdir}/acl.conf"),
                         ("persistence_location ", f"persistence_location {wdir}/"),
                         ("log_dest ", f"log_dest file {wdir}/mosquitto.log")):
            if line.startswith(key):
                line = val
        conf.append(line)
    (out / "mosquitto.conf").write_text("\n".join(conf) + "\n", newline="\n")
    r = wsl("sh", "-c", f"cp {src}/acl.conf {src}/passwd {src}/mosquitto.conf {wdir}/ && chmod 600 {wdir}/*")
    assert r.returncode == 0, r.stderr
    proc = subprocess.Popen(["wsl", "-d", DISTRO, "-e", "/usr/sbin/mosquitto", "-c", f"{wdir}/mosquitto.conf"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 15
        ready = False
        while time.time() < deadline and not ready:
            c = Client("bits_backend", passwords["bits_backend"])
            ready = c.ok
            c.close()
            if not ready:
                time.sleep(0.3)
        if not ready:
            log = wsl("cat", f"{wdir}/mosquitto.log").stdout[-500:]
            pytest.fail(f"isolated broker did not start: {log}")
        yield passwords
    finally:
        wsl("pkill", "-f", f"{wdir}/mosquitto.conf")
        proc.terminate()
        wsl("rm", "-rf", wdir)


class Client:
    def __init__(self, user=None, password=None, client_id="", wait=3.0):
        self.got = []
        self.rc = None
        self._conn, self._sub = threading.Event(), threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,
                             protocol=mqtt.MQTTv311)
        if user is not None:
            self.c.username_pw_set(user, password)
        self.c.on_connect = self._on_connect
        self.c.on_subscribe = lambda *a: self._sub.set()
        self.c.on_message = lambda c, u, m: self.got.append(m.topic)
        try:
            self.c.connect("127.0.0.1", PORT, keepalive=30)
        except OSError:
            self.rc = "oserr"
            self._conn.set()
            return
        self.c.loop_start()
        self._conn.wait(wait)

    def _on_connect(self, c, u, f, rc, props):
        self.rc = rc
        self._conn.set()

    @property
    def ok(self):
        return self.rc is not None and not isinstance(self.rc, str) and not self.rc.is_failure

    def sub(self, flt):
        self._sub.clear()
        self.c.subscribe(flt, 0)
        assert self._sub.wait(3), "no SUBACK"

    def pub(self, topic, payload="x"):
        info = self.c.publish(topic, payload, qos=1)
        info.wait_for_publish(3)

    def close(self):
        try:
            self.c.disconnect()
            self.c.loop_stop()
        except Exception:  # noqa: BLE001
            pass


@pytest.fixture
def make(broker):
    made = []

    def _make(user, client_id=""):
        c = Client(user, broker[user], client_id)
        made.append(c)
        assert c.ok, f"{user} failed to connect: {c.rc}"
        return c

    yield _make
    for c in made:
        c.close()


def delivered(subscriber, flt, publisher, topic, expect):
    """Subscriber listens on flt; publisher publishes topic; compare to expect."""
    subscriber.got.clear()
    subscriber.sub(flt)
    publisher.pub(topic)
    end = time.time() + (POS if expect else NEG)
    while time.time() < end:
        if topic in subscriber.got:
            break
        time.sleep(0.02)
    got = topic in subscriber.got
    assert got == expect, f"{topic}: delivered={got}, expected {expect}"


def test_conf_has_no_anonymous():
    require_templates()
    text = (DEPLOY / "bits.conf").read_text()
    active = [l for l in text.splitlines() if not l.lstrip().startswith("#")]
    assert not any(re.match(r"\s*allow_anonymous\s+true", l) for l in active)
    assert sum(l.startswith("listener ") for l in active) == 1


def test_anonymous_and_bad_password_refused(broker):
    assert not Client().ok
    assert not Client("bits_backend", "wrong").ok


def test_client_id_forced_to_username(make):
    r1 = make("dev_rly1")
    time.sleep(0.2)
    # Same requested client_id as rly1; without override the broker would kick rly1.
    r2 = make("dev_rly2", client_id="dev_rly1")
    time.sleep(0.8)
    assert r1.c.is_connected() and r2.c.is_connected()


def test_sender_own_topics_allowed(make):
    s1, be = make("dev_snd1"), make("bits_backend")
    delivered(be, "cas/+/status", s1, "cas/snd1/status", True)
    delivered(be, "cas/+/telemetry/#", s1, "cas/snd1/telemetry/weight", True)


def test_sender_cannot_publish_other_device_or_commands(make):
    s1, be, r1 = make("dev_snd1"), make("bits_backend"), make("dev_rly1")
    delivered(be, "cas/+/status", s1, "cas/snd2/status", False)
    delivered(be, "cas/+/telemetry/#", s1, "cas/rly1/telemetry/live", False)
    delivered(r1, "cas/rly1/commands", s1, "cas/rly1/commands", False)
    delivered(s1, "cas/snd1/commands", s1, "cas/snd1/commands", False)


def test_sender_publishes_weight_ctl_only_for_self(make):
    s1, r1, r2 = make("dev_snd1"), make("dev_rly1"), make("dev_rly2")
    delivered(r1, "cas/snd1/weight/ctl", s1, "cas/snd1/weight/ctl", True)
    delivered(r2, "cas/snd2/weight/ctl", s1, "cas/snd2/weight/ctl", False)


def test_sender_reads_own_commands_only(make):
    s1, be = make("dev_snd1"), make("bits_backend")
    delivered(s1, "cas/snd1/commands", be, "cas/snd1/commands", True)
    delivered(s1, "cas/+/commands", be, "cas/snd2/commands", False)


def test_relay_reads_only_its_sender_weight(make):
    s1, s2, r1 = make("dev_snd1"), make("dev_snd2"), make("dev_rly1")
    delivered(r1, "cas/snd2/weight/ctl", s2, "cas/snd2/weight/ctl", False)
    delivered(r1, "cas/+/weight/ctl", s2, "cas/snd2/weight/ctl", False)
    delivered(r1, "cas/snd1/weight/ctl", s1, "cas/snd1/weight/ctl", True)


def test_relay_cannot_publish_weight_ctl(make):
    r1, r2 = make("dev_rly1"), make("dev_rly2")
    delivered(r1, "cas/snd1/weight/ctl", r1, "cas/snd1/weight/ctl", False)
    delivered(r2, "cas/snd2/weight/ctl", r1, "cas/snd2/weight/ctl", False)


def test_relay_own_topics_and_isolation(make):
    r1, be = make("dev_rly1"), make("bits_backend")
    delivered(be, "cas/+/commands/ack", r1, "cas/rly1/commands/ack", True)
    delivered(be, "cas/+/run/#", r1, "cas/rly1/run/x", True)
    delivered(be, "cas/+/status", r1, "cas/rly2/status", False)
    delivered(r1, "cas/rly1/commands", be, "cas/rly1/commands", True)
    delivered(r1, "cas/rly2/commands", be, "cas/rly2/commands", False)
    delivered(r1, "cas/+/commands", be, "cas/rly2/commands", False)


def test_backend_has_no_weight_ctl_access(make):
    be, s1, r1 = make("bits_backend"), make("dev_snd1"), make("dev_rly1")
    delivered(be, "cas/+/weight/ctl", s1, "cas/snd1/weight/ctl", False)
    delivered(be, "cas/#", s1, "cas/snd1/weight/ctl", False)
    delivered(r1, "cas/snd1/weight/ctl", be, "cas/snd1/weight/ctl", False)


def test_backend_publishes_commands_not_status(make):
    be, r1 = make("bits_backend"), make("dev_rly1")
    delivered(r1, "cas/rly1/commands", be, "cas/rly1/commands", True)
    delivered(be, "cas/+/status", be, "cas/rly1/status", False)


def test_app_has_no_cas_access(make):
    a1, r1, be = make("app_st1"), make("dev_rly1"), make("bits_backend")
    delivered(r1, "cas/rly1/commands", a1, "cas/rly1/commands", False)
    delivered(a1, "cas/#", r1, "cas/rly1/status", False)
    delivered(a1, "cas/#", be, "cas/rly1/commands", False)


def test_app_req_and_res_scoped_to_own_client_id(make):
    a1, be = make("app_st1"), make("bits_backend")
    delivered(be, "bits/v1/ops/req/#", a1, "bits/v1/ops/req/app_st1/q", True)
    delivered(be, "bits/v1/ops/req/#", a1, "bits/v1/ops/req/app_st2/q", False)
    delivered(a1, "bits/v1/ops/res/app_st1/#", be, "bits/v1/ops/res/app_st1/r", True)
    delivered(a1, "bits/v1/ops/res/#", be, "bits/v1/ops/res/app_st2/r", False)
    delivered(a1, "bits/v1/ops/state/#", be, "bits/v1/ops/state/s", True)
    delivered(a1, "bits/v1/ops/evt/#", be, "bits/v1/ops/evt/e", True)
    # app cannot forge responses/state
    delivered(a1, "bits/v1/ops/state/#", a1, "bits/v1/ops/state/s", False)


def test_app_cannot_borrow_other_client_id(make):
    a1, be = make("app_st1", client_id="app_st2"), make("bits_backend")
    delivered(a1, "bits/v1/ops/res/app_st2/#", be, "bits/v1/ops/res/app_st2/r", False)
    delivered(be, "bits/v1/ops/req/#", a1, "bits/v1/ops/req/app_st2/q", False)


def test_sender_command_ack_allowed_for_self_only(make):
    s1, be, r1 = make("dev_snd1"), make("bits_backend"), make("dev_rly1")
    delivered(be, "cas/+/commands/ack", s1, "cas/snd1/commands/ack", True)
    delivered(be, "cas/+/commands/ack", s1, "cas/snd2/commands/ack", False)
    delivered(be, "cas/+/commands/ack", s1, "cas/rly1/commands/ack", False)
    # least privilege is unchanged: still no commands, run/#, or other devices' topics
    delivered(r1, "cas/rly1/commands", s1, "cas/rly1/commands", False)
    delivered(be, "cas/+/run/#", s1, "cas/snd1/run/x", False)
    # and nobody else gained the grant: a relay still cannot ack as a sender
    delivered(be, "cas/+/commands/ack", r1, "cas/snd1/commands/ack", False)
