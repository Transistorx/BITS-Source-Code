"""Process control for the integration tests: isolated mosquitto (WSL), headless backend
subprocess, seeded SQLite file, DB assertions. Never touches the real broker or database.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from sqlalchemy import create_engine, select, text
from sqlalchemy.orm import sessionmaker
from sqlalchemy.pool import NullPool

SERVER_DIR = Path(__file__).resolve().parents[2]
ROOT = SERVER_DIR.parent
DEPLOY = ROOT / "deploy" / "mosquitto"
PORT = 18831
DISTRO = "Ubuntu"
PW = "correct-horse-1"

INV = {"senders": ["snd1", "snd2"],
       "relays": [{"id": "rly1", "sender": "snd1"}, {"id": "rly2", "sender": "snd2"}],
       "stations": ["st1", "st2"]}


def wsl(*args, timeout=30):
    return subprocess.run(["wsl", "-d", DISTRO, "-e", *args], capture_output=True, text=True,
                          timeout=timeout)


def wait_for(cond, timeout=5.0, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()


class Broker:
    """Isolated mosquitto in WSL on PORT with the generated ACL (plus a probe user)."""

    # CONTRACT 9.7 says dev_<sender> may publish cas/<S>/# except cas/<S>/commands, but

    def __init__(self, tmp: Path):
        sys.path.insert(0, str(DEPLOY))
        import gen_config
        self.tmp = tmp
        inv, out, sec = tmp / "inv.json", tmp / "out", tmp / "secrets.json"
        inv.write_text(json.dumps(INV))
        subprocess.run([sys.executable, str(DEPLOY / "gen_config.py"), "--inventory", str(inv),
                        "--out-dir", str(out), "--secrets", str(sec)], check=True, capture_output=True)
        self.passwords = json.loads(sec.read_text())
        self.passwords["sim_probe"] = "probe-" + os.urandom(6).hex()
        acl = (out / "acl.conf").read_text()
        acl += "\nuser sim_probe\ntopic read cas/#\ntopic read bits/#\n"
        (out / "acl.conf").write_text(acl, newline="\n")
        with (out / "passwd").open("a", newline="\n") as f:
            f.write(f"sim_probe:{gen_config.hash_password(self.passwords['sim_probe'])}\n")
        self.wdir = wsl("mktemp", "-d", "/tmp/bits-it.XXXXXX").stdout.strip()
        assert self.wdir.startswith("/tmp/bits-it."), "could not create WSL temp dir"
        conf = []
        for line in (DEPLOY / "bits.conf").read_text().splitlines():
            for key, val in (("listener 1883", f"listener {PORT}"),
                             ("password_file ", f"password_file {self.wdir}/passwd"),
                             ("acl_file ", f"acl_file {self.wdir}/acl.conf"),
                             ("persistence_location ", f"persistence_location {self.wdir}/"),
                             ("autosave_interval ", "autosave_interval 1"),
                             ("log_dest ", f"log_dest file {self.wdir}/mosquitto.log")):
                if line.startswith(key):
                    line = val
            conf.append(line)
        (out / "mosquitto.conf").write_text("\n".join(conf) + "\n", newline="\n")
        src = wsl("wslpath", "-u", out.as_posix()).stdout.strip()
        r = wsl("sh", "-c", f"cp {src}/acl.conf {src}/passwd {src}/mosquitto.conf {self.wdir}/ "
                            f"&& chmod 600 {self.wdir}/*")
        assert r.returncode == 0, r.stderr
        self.conf = f"{self.wdir}/mosquitto.conf"
        self.proc: subprocess.Popen | None = None

    # -- control ------------------------------------------------------------------
    def start(self, timeout=15.0) -> None:
        self.proc = subprocess.Popen(["wsl", "-d", DISTRO, "-e", "/usr/sbin/mosquitto", "-c", self.conf],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for(self.accepting, timeout, 0.2):
            log = wsl("cat", f"{self.wdir}/mosquitto.log").stdout[-600:]
            raise RuntimeError(f"isolated broker did not start: {log}")

    def accepting(self) -> bool:
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.5):
                return True
        except OSError:
            return False

    def kill9(self) -> None:
        """kill -9 of the broker process (no persistence flush, no goodbye to clients)."""
        wsl("pkill", "-9", "-f", self.conf)
        if self.proc:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        assert wait_for(lambda: not self.accepting(), 8.0), "broker still listening after kill -9"

    def stop(self) -> None:
        """SIGTERM: persistence is written, retained messages survive the restart."""
        wsl("pkill", "-TERM", "-f", self.conf)
        if self.proc:
            try:
                self.proc.wait(8)
            except subprocess.TimeoutExpired:
                self.kill9()

    def restart(self, hard: bool = True) -> None:
        (self.kill9 if hard else self.stop)()
        self.start()

    def reset(self) -> None:
        """Fresh broker: no persisted retained messages from a previous test."""
        if self.accepting():
            self.kill9()
        wsl("sh", "-c", f"rm -f {self.wdir}/mosquitto.db")
        self.start()

    def close(self) -> None:
        wsl("pkill", "-9", "-f", self.conf)
        if self.proc:
            self.proc.kill()
        wsl("rm", "-rf", self.wdir)


def broker_available() -> str | None:
    """None if usable, else the skip reason."""
    if not shutil.which("wsl"):
        return "wsl not available"
    try:
        r = wsl("sh", "-c", "test -x /usr/sbin/mosquitto")
    except Exception as exc:  # noqa: BLE001
        return f"wsl unusable: {exc}"
    if r.returncode != 0:
        return f"mosquitto not installed in WSL distro {DISTRO}"
    with socket.socket() as s:
        s.settimeout(0.3)
        if s.connect_ex(("127.0.0.1", PORT)) == 0:
            return f"port {PORT} already in use"
    return None


class Backend:
    """`python -m app.service` as a real subprocess (so it can be kill -9'd)."""

    def __init__(self, db_url: str, broker: Broker, logfile: Path, **env):
        self.db_url, self.broker, self.logfile, self.extra = db_url, broker, logfile, env
        self.proc: subprocess.Popen | None = None
        self.starts = 0

    def env(self) -> dict:
        e = dict(os.environ)
        e.update(DATABASE_URL=self.db_url, MQTT_ENABLED="false", MQTT_BROKER_HOST="127.0.0.1",
                 MQTT_BROKER_PORT=str(PORT), MQTT_USERNAME="bits_backend",
                 MQTT_PASSWORD=self.broker.passwords["bits_backend"], MQTT_CLIENT_ID="bits_backend",
                 LOG_LEVEL="INFO", PYTHONUNBUFFERED="1", API_KEY="",
                 RUN_STALE_SECONDS=e.get("RUN_STALE_SECONDS", "3"))
        e.update({k: str(v) for k, v in self.extra.items()})
        return e

    def start(self) -> None:
        assert self.proc is None or self.proc.poll() is not None, "backend already running"
        self.starts += 1
        log = self.logfile.open("ab")
        self.proc = subprocess.Popen([sys.executable, "-m", "app.service"], cwd=str(SERVER_DIR),
                                     env=self.env(), stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def kill9(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.kill()                     # TerminateProcess: nothing is flushed
            self.proc.wait(10)

    def stop(self) -> None:
        """Graceful (CTRL_BREAK -> SIGBREAK -> request_stop -> drain)."""
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.CTRL_BREAK_EVENT)
                self.proc.wait(20)
            except Exception:  # noqa: BLE001
                self.kill9()

    def log_tail(self, n=1500) -> str:
        try:
            return self.logfile.read_text(errors="replace")[-n:]
        except OSError:
            return ""


class DB:
    """Short-lived read connections to the backend's SQLite file."""

    def __init__(self, url: str):
        self.url = url
        self.engine = create_engine(url, poolclass=NullPool, future=True,
                                    connect_args={"timeout": 30})
        self.Session = sessionmaker(bind=self.engine, expire_on_commit=False, future=True)

    def all(self, model, *where, order=None):
        with self.Session() as s:
            q = select(model).where(*where)
            if order is not None:
                q = q.order_by(order)
            return list(s.scalars(q))

    def get(self, model, key):
        with self.Session() as s:
            return s.get(model, key)

    def exec(self, sql: str, **params):
        with self.engine.begin() as c:
            return c.execute(text(sql), params)

    def scalar(self, sql: str, **params):
        with self.engine.connect() as c:
            return c.execute(text(sql), params).scalar()
