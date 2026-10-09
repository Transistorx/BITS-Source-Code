import logging
import ssl

import paho.mqtt.client as paho_client
import pytest

from app import mqtt_bridge

SECRET = "tls-s3cr3t-pw-q9"


class FakeTlsClient:
    instances = []

    def __init__(self, *args, **kwargs):
        self.kwargs = kwargs
        self.tls_set_calls = []
        self.tls_insecure_calls = []
        self.tls_context_calls = []
        self.connect_calls = []
        FakeTlsClient.instances.append(self)

    def tls_set(self, **kwargs):
        self.tls_set_calls.append(kwargs)
        version = kwargs.get("tls_version") or ssl.PROTOCOL_TLS_CLIENT
        ctx = ssl.SSLContext(version)
        ctx.verify_mode = kwargs.get("cert_reqs") or ssl.CERT_REQUIRED
        self._ssl_context = ctx
        self.tls_insecure_calls.append(False)

    def tls_insecure_set(self, value):
        self.tls_insecure_calls.append(value)

    def tls_set_context(self, context=None):
        self.tls_context_calls.append(context)

    def connect_async(self, host, port, **kwargs):
        self.connect_calls.append((host, port))

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda *a, **k: None


@pytest.fixture()
def fake_tls(monkeypatch):
    FakeTlsClient.instances = []
    monkeypatch.setattr(paho_client, "Client", FakeTlsClient)
    monkeypatch.setattr(mqtt_bridge, "_client", None)
    monkeypatch.setenv("MQTT_ENABLED", "true")
    monkeypatch.delenv("WEB_CONCURRENCY", raising=False)
    monkeypatch.delenv("MQTT_TLS", raising=False)
    monkeypatch.delenv("MQTT_TLS_CA_FILE", raising=False)
    yield FakeTlsClient
    mqtt_bridge.stop()


def _ca_file(tmp_path):
    path = tmp_path / "ca.pem"
    path.write_text("-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n")
    return path


def test_tls_off_by_default_no_tls_calls(fake_tls):
    mqtt_bridge.start()
    client = fake_tls.instances[0]
    assert client.tls_set_calls == []
    assert client.tls_insecure_calls == []
    assert client.tls_context_calls == []
    assert mqtt_bridge._client is client


def test_tls_false_explicit_no_tls_calls_even_with_ca_set(monkeypatch, fake_tls, tmp_path):
    monkeypatch.setenv("MQTT_TLS", "false")
    monkeypatch.setenv("MQTT_TLS_CA_FILE", str(tmp_path / "missing.pem"))
    mqtt_bridge.start()
    client = fake_tls.instances[0]
    assert client.tls_set_calls == []
    assert mqtt_bridge._client is client


def test_tls_on_without_ca_uses_system_cas_with_verification(monkeypatch, fake_tls):
    monkeypatch.setenv("MQTT_TLS", "true")
    mqtt_bridge.start()
    client = fake_tls.instances[0]
    assert len(client.tls_set_calls) == 1
    call = client.tls_set_calls[0]
    assert call.get("ca_certs") is None
    assert call.get("cert_reqs") == ssl.CERT_REQUIRED
    assert call.get("tls_version") == ssl.PROTOCOL_TLS_CLIENT
    assert True not in client.tls_insecure_calls
    ctx = client._ssl_context
    assert ctx.verify_mode == ssl.CERT_REQUIRED
    assert ctx.check_hostname is True
    assert ctx.minimum_version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3)
    assert mqtt_bridge._client is client


def test_tls_on_with_ca_passes_ca_certs(monkeypatch, fake_tls, tmp_path):
    ca = _ca_file(tmp_path)
    monkeypatch.setenv("MQTT_TLS", "true")
    monkeypatch.setenv("MQTT_TLS_CA_FILE", str(ca))
    mqtt_bridge.start()
    client = fake_tls.instances[0]
    assert len(client.tls_set_calls) == 1
    assert client.tls_set_calls[0].get("ca_certs") == str(ca)
    assert client.tls_set_calls[0].get("cert_reqs") == ssl.CERT_REQUIRED
    assert True not in client.tls_insecure_calls


def test_tls_on_missing_ca_file_fails_fast(monkeypatch, fake_tls, tmp_path):
    missing = tmp_path / "nope.pem"
    monkeypatch.setenv("MQTT_TLS", "true")
    monkeypatch.setenv("MQTT_TLS_CA_FILE", str(missing))
    with pytest.raises(RuntimeError) as exc:
        mqtt_bridge.start()
    assert "MQTT_TLS_CA_FILE" in str(exc.value)
    assert mqtt_bridge._client is None
    assert mqtt_bridge._publisher is None
    assert all(c.connect_calls == [] for c in fake_tls.instances)


def test_tls_on_ca_path_is_directory_fails_fast(monkeypatch, fake_tls, tmp_path):
    monkeypatch.setenv("MQTT_TLS", "true")
    monkeypatch.setenv("MQTT_TLS_CA_FILE", str(tmp_path))
    with pytest.raises(RuntimeError) as exc:
        mqtt_bridge.start()
    assert "MQTT_TLS_CA_FILE" in str(exc.value)
    assert mqtt_bridge._client is None


def test_tls_setup_error_fails_closed_never_plaintext(monkeypatch, fake_tls):
    monkeypatch.setenv("MQTT_TLS", "true")

    def broken(self, **kwargs):
        raise ssl.SSLError("bad ca")

    monkeypatch.setattr(FakeTlsClient, "tls_set", broken)
    with pytest.raises(RuntimeError):
        mqtt_bridge.start()
    assert mqtt_bridge._client is None
    assert all(c.connect_calls == [] for c in fake_tls.instances)


def test_tls_on_real_paho_context_is_verified_tls12_min(monkeypatch, tmp_path):
    monkeypatch.setenv("MQTT_TLS", "true")
    monkeypatch.delenv("MQTT_TLS_CA_FILE", raising=False)
    client = paho_client.Client(paho_client.CallbackAPIVersion.VERSION2, client_id="tls-test")
    mqtt_bridge.apply_tls(client)
    ctx = client._ssl_context
    assert isinstance(ctx, ssl.SSLContext)
    assert ctx.verify_mode == ssl.CERT_REQUIRED
    assert ctx.check_hostname is True
    assert ctx.minimum_version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3)
    assert client._tls_insecure is False


def test_password_never_in_logs_with_tls(monkeypatch, fake_tls, caplog, tmp_path):
    caplog.set_level(logging.DEBUG)
    monkeypatch.setenv("MQTT_USERNAME", "bits")
    monkeypatch.setenv("MQTT_PASSWORD", SECRET)
    monkeypatch.setenv("MQTT_TLS", "true")
    mqtt_bridge.start()
    mqtt_bridge.stop()
    monkeypatch.setattr(mqtt_bridge, "_client", None)
    monkeypatch.setenv("MQTT_TLS_CA_FILE", str(tmp_path / "missing.pem"))
    with pytest.raises(RuntimeError) as exc:
        mqtt_bridge.start()
    assert SECRET not in str(exc.value)
    assert SECRET not in caplog.text
