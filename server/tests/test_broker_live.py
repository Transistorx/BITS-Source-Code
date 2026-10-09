"""ACL / TLS / auth matrix against a RUNNING Mosquitto (e.g. the Windows service), not an isolated one.

Skipped unless BITS_BROKER_SECRETS points at the mqtt-users.json written by gen_config.py.
  BITS_BROKER_HOST    default 127.0.0.1
  BITS_BROKER_PORT    default 1885 (plaintext dev listener) ; use 8883 with BITS_BROKER_CA
  BITS_BROKER_CA      path to ca.crt -> TLS with CA verification
  BITS_BROKER_SECRETS path to mqtt-users.json (never printed)
Inventory expected: sender snd1, relay rly1 (peer snd1), station ws1, admin bits_admin.
Denied publishes are silent, so denial = non-delivery to an authorised subscriber.
Run:  pytest -m live_broker tests/test_broker_live.py
"""
import json
import os
import ssl
import threading
import time
import uuid
from pathlib import Path

import pytest

mqtt = pytest.importorskip("paho.mqtt.client")

SECRETS = os.environ.get("BITS_BROKER_SECRETS")
HOST = os.environ.get("BITS_BROKER_HOST", "127.0.0.1")
CA = os.environ.get("BITS_BROKER_CA")
PORT = int(os.environ.get("BITS_BROKER_PORT", "8883" if CA else "1885"))
NEG, POS = 0.5, 4.0

pytestmark = [pytest.mark.live_broker,
              pytest.mark.skipif(not SECRETS, reason="BITS_BROKER_SECRETS not set")]


class Client:
    def __init__(self, user=None, password=None, client_id="", host=None, port=None, ca="default",
                 wait=5.0):
        self.got, self.rc = [], None
        self._conn, self._sub = threading.Event(), threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,
                             protocol=mqtt.MQTTv311)
        ca = CA if ca == "default" else ca
        if ca:
            self.c.tls_set(ca_certs=ca, cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        if user is not None:
            self.c.username_pw_set(user, password)
        self.c.on_connect = self._on_connect
        self.c.on_subscribe = lambda *a: self._sub.set()
        self.c.on_message = lambda c, u, m: self.got.append(m.topic)
        try:
            self.c.connect(host or HOST, port or PORT, keepalive=30)
        except (OSError, ssl.SSLError) as e:
            self.rc = f"err:{type(e).__name__}"
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

    def pub(self, topic, payload="x", retain=False):
        self.c.publish(topic, payload, qos=1, retain=retain).wait_for_publish(3)

    def close(self):
        try:
            self.c.disconnect()
            self.c.loop_stop()
        except Exception:  # noqa: BLE001
            pass


@pytest.fixture(scope="module")
def pw():
    return json.loads(Path(SECRETS).read_text())


@pytest.fixture
def make(pw):
    made = []

    def _make(user, client_id=""):
        c = Client(user, pw[user], client_id)
        made.append(c)
        assert c.ok, f"{user} failed to connect: {c.rc}"
        return c

    yield _make
    for c in made:
        c.close()


def delivered(sub, flt, pub, topic, expect):
    sub.got.clear()
    sub.sub(flt)
    pub.pub(topic)
    end = time.time() + (POS if expect else NEG)
    while time.time() < end and topic not in sub.got:
        time.sleep(0.02)
    assert (topic in sub.got) == expect, f"{topic}: delivered={topic in sub.got}, expected {expect}"


def test_anonymous_and_wrong_password_refused(pw):
    a = Client()
    assert not a.ok
    a.close()
    b = Client("bits_backend", "wrong")
    assert not b.ok
    b.close()


@pytest.mark.skipif(not CA, reason="TLS only")
def test_tls_wrong_ca_and_wrong_hostname_fail(pw, tmp_path):
    # A CA that did not sign the server cert must be rejected.
    import subprocess
    other = tmp_path / "other.crt"
    key = tmp_path / "o.key"
    git_ossl = r"C:\Program Files\Git\usr\bin\openssl.exe"
    if not os.path.exists(git_ossl):
        pytest.skip("openssl not found to make a foreign CA")
    subprocess.run([git_ossl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-keyout", str(key), "-out", str(other), "-subj", "/CN=Other", "-days", "2"],
                   check=True, capture_output=True)
    c = Client("bits_backend", pw["bits_backend"], ca=str(other))
    assert not c.ok and str(c.rc).startswith("err:")
    c.close()
    # Hostname not in the SAN: connect by an address the cert does not cover (loopback alias).
    c = Client("bits_backend", pw["bits_backend"], host="127.0.0.2")
    assert not c.ok
    c.close()


@pytest.mark.skipif(not CA, reason="TLS only")
def test_plaintext_client_cannot_use_tls_port(pw):
    c = Client("bits_backend", pw["bits_backend"], ca=None, wait=2.0)
    assert not c.ok
    c.close()


def test_client_id_forced_to_username(make):
    r1 = make("dev_rly1")
    time.sleep(0.2)
    s1 = make("dev_snd1", client_id="dev_rly1")
    time.sleep(0.8)
    assert r1.c.is_connected() and s1.c.is_connected()


def test_sender_acl(make):
    s1, be, r1 = make("dev_snd1"), make("bits_backend"), make("dev_rly1")
    delivered(be, "cas/+/status", s1, "cas/snd1/status", True)
    delivered(be, "cas/+/telemetry/#", s1, "cas/snd1/telemetry/weight", True)
    delivered(r1, "cas/snd1/weight/ctl", s1, "cas/snd1/weight/ctl", True)
    delivered(be, "cas/+/status", s1, "cas/snd2/status", False)
    delivered(r1, "cas/rly1/commands", s1, "cas/rly1/commands", False)
    delivered(be, "cas/+/run/#", s1, "cas/snd1/run/x", False)


def test_relay_acl(make):
    r1, be, s1 = make("dev_rly1"), make("bits_backend"), make("dev_snd1")
    delivered(be, "cas/+/commands/ack", r1, "cas/rly1/commands/ack", True)
    delivered(be, "cas/+/run/#", r1, "cas/rly1/run/x", True)
    delivered(r1, "cas/rly1/commands", be, "cas/rly1/commands", True)
    delivered(be, "cas/+/status", r1, "cas/rly2/status", False)
    delivered(r1, "cas/snd1/weight/ctl", r1, "cas/snd1/weight/ctl", False)  # relay cannot publish ctl


def test_backend_acl(make):
    be, s1, r1 = make("bits_backend"), make("dev_snd1"), make("dev_rly1")
    delivered(be, "cas/#", s1, "cas/snd1/weight/ctl", False)
    delivered(r1, "cas/snd1/weight/ctl", be, "cas/snd1/weight/ctl", False)
    delivered(be, "cas/+/status", be, "cas/rly1/status", False)


def test_station_acl(make):
    a, r1, be = make("app_ws1"), make("dev_rly1"), make("bits_backend")
    delivered(r1, "cas/rly1/commands", a, "cas/rly1/commands", False)
    delivered(a, "cas/#", be, "cas/rly1/commands", False)
    delivered(be, "bits/v1/ops/req/#", a, "bits/v1/ops/req/app_ws1/q", True)
    delivered(be, "bits/v1/ops/req/#", a, "bits/v1/ops/req/app_ws2/q", False)
    delivered(a, "bits/v1/ops/res/app_ws1/#", be, "bits/v1/ops/res/app_ws1/r", True)
    delivered(a, "bits/v1/ops/state/#", a, "bits/v1/ops/state/s", False)


def test_admin_is_read_only(make):
    ad, be, s1 = make("bits_admin"), make("bits_backend"), make("dev_snd1")
    delivered(ad, "cas/#", s1, "cas/snd1/status", True)
    delivered(be, "cas/+/commands/ack", ad, "cas/snd1/commands/ack", False)
    delivered(be, "bits/v1/ops/req/#", ad, "bits/v1/ops/req/app_ws1/q", False)
    ad.got.clear()
    ad.sub("$SYS/broker/uptime")
    end = time.time() + 12
    while time.time() < end and not ad.got:
        time.sleep(0.2)
    assert "$SYS/broker/uptime" in ad.got


def test_retained_status_ok_commands_not_retained(make):
    topic = f"cas/snd1/status"
    s1, be = make("dev_snd1"), make("bits_backend")
    s1.pub(topic, "online", retain=True)
    late = make("bits_admin")
    late.got.clear()
    late.sub(topic)
    time.sleep(0.5)
    assert topic in late.got
    s1.pub(topic, "", retain=True)  # clear
    # commands: backend publishes (non-retained); a later subscriber must see nothing
    be.pub("cas/rly1/commands", "{}")
    r1 = make("dev_rly1")
    r1.got.clear()
    r1.sub("cas/rly1/commands")
    time.sleep(0.5)
    assert "cas/rly1/commands" not in r1.got


def test_qos1_roundtrip(make):
    be, r1 = make("bits_backend"), make("dev_rly1")
    r1.sub("cas/rly1/commands")
    t0 = time.perf_counter()
    be.pub("cas/rly1/commands", uuid.uuid4().hex)
    while "cas/rly1/commands" not in r1.got and time.perf_counter() - t0 < 3:
        time.sleep(0.001)
    assert "cas/rly1/commands" in r1.got
    assert time.perf_counter() - t0 < 1.0
