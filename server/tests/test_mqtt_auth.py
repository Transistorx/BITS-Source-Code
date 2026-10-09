"""MQTT broker auth: username_pw_set only when configured; password never logged."""

import logging

import paho.mqtt.client as paho_client

from app import mqtt_bridge
from app.config import settings

SECRET = "s3cr3t-pw-xyz"


class FakeClient:
    instances = []

    def __init__(self, *a, **k):
        self.calls = []
        FakeClient.instances.append(self)

    def username_pw_set(self, username, password=None):
        self.calls.append((username, password))

    def __getattr__(self, name):
        return lambda *a, **k: None


def _start(monkeypatch, env):
    FakeClient.instances.clear()
    monkeypatch.setattr(paho_client, "Client", FakeClient)
    monkeypatch.setenv("MQTT_ENABLED", "true")
    for k, v in env.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setattr(mqtt_bridge, "_client", None)
    try:
        mqtt_bridge.start()
    finally:
        mqtt_bridge.stop()
    return FakeClient.instances[0]


def test_username_pw_set_called_when_set(monkeypatch):
    c = _start(monkeypatch, {"MQTT_USERNAME": "bits", "MQTT_PASSWORD": SECRET})
    assert c.calls == [("bits", SECRET)]


def test_username_pw_set_not_called_when_unset(monkeypatch):
    monkeypatch.delenv("MQTT_USERNAME", raising=False)
    monkeypatch.delenv("MQTT_PASSWORD", raising=False)
    c = _start(monkeypatch, {})
    assert c.calls == []


def test_password_never_in_logs(monkeypatch, caplog):
    caplog.set_level(logging.DEBUG)
    _start(monkeypatch, {"MQTT_USERNAME": "bits", "MQTT_PASSWORD": SECRET})
    assert SECRET not in caplog.text
    assert SECRET not in repr(settings)
