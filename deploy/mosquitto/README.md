# Mosquitto broker template (REQ-WMQ-29, HZ-10)

The broker ACL is the only authentication of ZERO/TARE and relay commands on the
MQTT side. These files are templates: they contain no credentials. Generated
`passwd` and `mqtt-users.json` are git-ignored and must stay out of the repo.

Files:

- `bits.conf` - broker config: `allow_anonymous false`, one plaintext LAN
  listener on 1883, `use_username_as_clientid true` (a client cannot borrow
  another identity's client id; the firmware sets client_id = device_id, which
  the broker replaces with the username), password file and ACL file.
- `acl.conf` - ACL template. Blocks whose `user` line contains `{sender}`,
  `{relay}` or `{station}` are repeated per inventory entry; `{sender}` inside a
  relay block is that relay's peer sender. Rules before the first `user` line
  and `pattern` rules are refused by the renderer because they would grant
  default access to every client.
- `gen_config.py` - renders `acl.conf` and `passwd` from an inventory and writes
  or reuses the plaintext passwords in a 0600 secrets file.

## Identities

| user | may publish | may subscribe |
|---|---|---|
| `bits_backend` (server) | `cas/+/commands`, `cas/+/run/ack`, `bits/v1/ops/{res,state,evt}/#` | `cas/+/status`, `cas/+/telemetry/#`, `cas/+/commands/ack`, `cas/+/run/#`, `bits/v1/ops/req/#` |
| `dev_<sender>` (weight sender) | `cas/<sender>/status`, `cas/<sender>/telemetry/#`, `cas/<sender>/weight/#`, `cas/<sender>/commands/ack` | `cas/<sender>/commands` |
| `dev_<relay>` (relay, peer `<sender>`) | `cas/<relay>/status`, `cas/<relay>/telemetry/#`, `cas/<relay>/commands/ack`, `cas/<relay>/run/#` | `cas/<relay>/commands`, `cas/<relay>/run/ack`, `cas/<sender>/weight/ctl` |
| `app_<station>` (operator app) | `bits/v1/ops/req/app_<station>/#` | `bits/v1/ops/res/app_<station>/#`, `bits/v1/ops/state/#`, `bits/v1/ops/evt/#` |
| `bits_admin` | nothing | `cas/#`, `bits/#`, `$SYS/#` |

Nobody except `bits_backend` can publish to any `.../commands` topic, so a
third party on the LAN with a device credential cannot tare another scale or
set a retained command.

## Generate

```bash
cat > inventory.json <<'EOF'
{"senders": ["snd1"], "relays": [{"id": "rly1", "sender": "snd1"}], "stations": ["ws1"]}
EOF
python3 deploy/mosquitto/gen_config.py --inventory inventory.json \
    --out-dir /tmp/bits-broker --secrets /root/mqtt-users.json
```

Identities are `[A-Za-z0-9_-]{1,32}`; anything else (slash, `+`, `#`, blank)
is refused and nothing is written. Re-running with the same secrets file keeps
existing passwords and only adds new identities. Passwords are never printed.
Hashes use mosquitto's `$7$` PBKDF2-SHA512 format (mosquitto 2.x).

## Install (Ubuntu, mosquitto 2.x)

```bash
sudo install -d -m 750 -o mosquitto -g mosquitto /etc/mosquitto/bits
sudo install -m 640 -o mosquitto -g mosquitto /tmp/bits-broker/acl.conf /etc/mosquitto/bits/acl.conf
sudo install -m 640 -o mosquitto -g mosquitto /tmp/bits-broker/passwd /etc/mosquitto/bits/passwd
sudo install -m 644 deploy/mosquitto/bits.conf /etc/mosquitto/conf.d/bits.conf
sudo systemctl restart mosquitto
```

Remove or disable any other `listener` or `allow_anonymous true` line in
`/etc/mosquitto/mosquitto.conf` and `conf.d/`; the template is only safe when
it is the sole listener definition. Give the server `MQTT_USERNAME=bits_backend`
and the matching password from the secrets file, and each device its
`dev_<id>` credential through provisioning (never in firmware source).

Verify with the repo tests: `python -m pytest server/tests/test_broker_acl.py`
runs an isolated copy on port 18830 in WSL when mosquitto is installed there;
`server/tests/test_broker_live.py` checks a running broker when
`BITS_BROKER_SECRETS` points at the secrets file.

## Optional TLS listener (8883)

Keep `bits.conf` unchanged (the tests require exactly one listener there) and
add a second file, for example `/etc/mosquitto/conf.d/bits-tls.conf`:

```
listener 8883
protocol mqtt
cafile /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/broker.crt
keyfile /etc/mosquitto/certs/broker.key
tls_version tlsv1.2
```

The ACL and password file are global, so the same identities apply on both
listeners. The certificate SAN must contain the broker's LAN address or name
exactly as the clients dial it. Server side set `MQTT_TLS=true`,
`MQTT_BROKER_PORT=8883`, `MQTT_TLS_CA_FILE=/path/ca.crt`. Once every client is
on 8883, delete `listener 1883` from `bits.conf` on that host only (the repo
template keeps 1883 for LAN bench use).

## Rollback

Delete `/etc/mosquitto/conf.d/bits.conf` (and `bits-tls.conf`) and restart
mosquitto to return to the previous broker configuration. Deployed devices and
the server keep working against any broker that still accepts their credentials;
no persistent state on the devices depends on this file.
