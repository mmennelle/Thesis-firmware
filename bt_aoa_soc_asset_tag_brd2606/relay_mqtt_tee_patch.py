#!/usr/bin/env python3
"""Idempotent in-place patches that let `system_test.py --injector hardware
--scenario relay` coexist with a running production stack.

Applied changes (all guarded by sentinel strings so re-running is a no-op):

  1. /opt/ble-thesis/cs_reader/cs_reader.py
       - new constructor args: raw_tee_enabled, raw_tee_topic_prefix
       - publish each raw VCOM line to silabs/cs/raw/<locator_id>
       - main() passes the new keys from config; CFG_DEFAULTS gains them.

  2. /opt/ble-thesis/cs_reader/cs_reader_config.json
       - adds raw_tee_enabled=true, raw_tee_topic_prefix="silabs/cs/raw".

  3. /opt/ble-thesis/security/cs_log_capture.py
       - InitiatorLogCapture._run sends GO\\n on every (re)connect.
       - new MqttLogCapture class with the same interface as
         InitiatorLogCapture; consumes silabs/cs/raw/<locator_id>.

  4. /opt/ble-thesis/security/system_test.py
       - Deployment.cs_log_via ("mqtt"|"tcp", default "mqtt") and
         mqtt_username / password_env (read from cs_reader config for
         convenience).
       - hardware-injector block instantiates MqttLogCapture when the
         deployment says so; the existing TCP path stays as the fallback.

  5. /etc/mosquitto/acl (sudo): grant `cs_reader` write to silabs/cs/raw/#.

After patching the script runs mosquitto -SIGHUP and restarts
cs-reader@ble-pd-0C4314F0319C only. No other services are touched.

Run on the Pi:
    python3 /tmp/relay_mqtt_tee_patch.py
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

CS_READER = Path("/opt/ble-thesis/cs_reader/cs_reader.py")
CS_READER_CFG = Path("/opt/ble-thesis/cs_reader/cs_reader_config.json")
CS_LOG_CAPTURE = Path("/opt/ble-thesis/security/cs_log_capture.py")
SYSTEM_TEST = Path("/opt/ble-thesis/security/system_test.py")
MOSQ_ACL = Path("/etc/mosquitto/acl")

CHANGES: list[str] = []


def backup(p: Path) -> None:
    bak = p.with_suffix(p.suffix + ".bak.relay_tee")
    if not bak.exists():
        shutil.copy2(p, bak)


def patch_file(p: Path, replacements: list[tuple[str, str, str]]) -> None:
    """`replacements` is a list of (sentinel, old, new). If sentinel is
    already in the file, the change is skipped."""
    src = p.read_text()
    changed = False
    out = src
    for sentinel, old, new in replacements:
        if sentinel in out:
            continue
        if old not in out:
            raise SystemExit(f"{p}: cannot find anchor for sentinel {sentinel!r}")
        out = out.replace(old, new, 1)
        changed = True
    if changed:
        backup(p)
        p.write_text(out)
        CHANGES.append(str(p))


# ──────────────────────────────────────────────────────────────────────────
# 1. cs_reader.py
# ──────────────────────────────────────────────────────────────────────────
CS_READER_ANCHOR_INIT_PARAMS = (
    '        username: str | None,\n'
    '        password: str | None,\n'
    '        log: logging.Logger,\n'
    '    ) -> None:'
)
CS_READER_NEW_INIT_PARAMS = (
    '        username: str | None,\n'
    '        password: str | None,\n'
    '        log: logging.Logger,\n'
    '        raw_tee_enabled: bool = False,\n'
    '        raw_tee_topic_prefix: str = "silabs/cs/raw",\n'
    '    ) -> None:'
)

CS_READER_ANCHOR_INIT_BODY = (
    '        self._topic_prefix = topic_prefix.rstrip("/")\n'
    '        self._mqtt_qos = int(mqtt_qos)\n'
    '        self._mqtt_retain = bool(mqtt_retain)\n'
    '        self._log = log\n'
)
CS_READER_NEW_INIT_BODY = (
    '        self._topic_prefix = topic_prefix.rstrip("/")\n'
    '        self._mqtt_qos = int(mqtt_qos)\n'
    '        self._mqtt_retain = bool(mqtt_retain)\n'
    '        self._log = log\n'
    '        # Raw-VCOM tee → MQTT (silabs/cs/raw/<locator_id>). Enables\n'
    '        # security/system_test.py InitiatorLogCapture (MQTT-backed) to\n'
    '        # observe the same line stream without contending for TCP 4901.\n'
    '        self._raw_tee_enabled = bool(raw_tee_enabled)\n'
    '        self._raw_tee_topic = f"{raw_tee_topic_prefix.rstrip(chr(47))}/{locator_id}"\n'
)

CS_READER_ANCHOR_HANDLE_LINE = (
    '    def _handle_line(self, line: str) -> None:\n'
    '        if not line:\n'
    '            return\n'
)
CS_READER_NEW_HANDLE_LINE = (
    '    def _handle_line(self, line: str) -> None:\n'
    '        if not line:\n'
    '            return\n'
    '        # Raw-tee FIRST: every non-empty line goes to MQTT exactly as\n'
    '        # received from VCOM. Best-effort, never raises.\n'
    '        if self._raw_tee_enabled:\n'
    '            try:\n'
    '                self._client.publish(self._raw_tee_topic, line, qos=0, retain=False)\n'
    '            except Exception:  # noqa: BLE001\n'
    '                pass\n'
)

CS_READER_ANCHOR_CFG_DEFAULTS = (
    '    "password": None,\n'
    '    "password_env": None,\n'
    '}\n'
)
CS_READER_NEW_CFG_DEFAULTS = (
    '    "password": None,\n'
    '    "password_env": None,\n'
    '    "raw_tee_enabled": True,\n'
    '    "raw_tee_topic_prefix": "silabs/cs/raw",\n'
    '}\n'
)

CS_READER_ANCHOR_MAIN = (
    '        username=cfg.get("username"),\n'
    '        password=_mqtt_password(cfg),\n'
    '        log=log,\n'
    '    )\n'
    '    reader.install_signal_handlers()'
)
CS_READER_NEW_MAIN = (
    '        username=cfg.get("username"),\n'
    '        password=_mqtt_password(cfg),\n'
    '        log=log,\n'
    '        raw_tee_enabled=bool(cfg.get("raw_tee_enabled", False)),\n'
    '        raw_tee_topic_prefix=str(cfg.get("raw_tee_topic_prefix", "silabs/cs/raw")),\n'
    '    )\n'
    '    reader.install_signal_handlers()'
)

patch_file(
    CS_READER,
    [
        ("raw_tee_enabled: bool", CS_READER_ANCHOR_INIT_PARAMS, CS_READER_NEW_INIT_PARAMS),
        ("self._raw_tee_enabled", CS_READER_ANCHOR_INIT_BODY, CS_READER_NEW_INIT_BODY),
        ("Raw-tee FIRST", CS_READER_ANCHOR_HANDLE_LINE, CS_READER_NEW_HANDLE_LINE),
        ('"raw_tee_enabled":', CS_READER_ANCHOR_CFG_DEFAULTS, CS_READER_NEW_CFG_DEFAULTS),
        ("raw_tee_enabled=bool(", CS_READER_ANCHOR_MAIN, CS_READER_NEW_MAIN),
    ],
)

# ──────────────────────────────────────────────────────────────────────────
# 2. cs_reader_config.json
# ──────────────────────────────────────────────────────────────────────────
cfg = json.loads(CS_READER_CFG.read_text())
cfg_changed = False
if "raw_tee_enabled" not in cfg:
    cfg["raw_tee_enabled"] = True
    cfg_changed = True
if "raw_tee_topic_prefix" not in cfg:
    cfg["raw_tee_topic_prefix"] = "silabs/cs/raw"
    cfg_changed = True
if cfg_changed:
    backup(CS_READER_CFG)
    CS_READER_CFG.write_text(json.dumps(cfg, indent=2) + "\n")
    CHANGES.append(str(CS_READER_CFG))

# ──────────────────────────────────────────────────────────────────────────
# 3. cs_log_capture.py — GO\n on every (re)connect + MqttLogCapture
# ──────────────────────────────────────────────────────────────────────────
CS_LOG_ANCHOR_GO = (
    '            backoff = self.backoff_initial_s\n'
    '            self._connected.set()\n'
    '            sock.settimeout(self.recv_timeout_s)\n'
    '            buf = b""\n'
)
CS_LOG_NEW_GO = (
    '            backoff = self.backoff_initial_s\n'
    '            self._connected.set()\n'
    '            sock.settimeout(self.recv_timeout_s)\n'
    '            # Unlock the WSTK J-Link UART-to-TCP write-gate. Idempotent;\n'
    '            # if cs_reader.service already opened the gate this is a\n'
    '            # no-op on the wire. See PIPELINE.md §2.5.\n'
    '            try:\n'
    '                sock.sendall(b"GO\\n")\n'
    '            except OSError:\n'
    '                pass\n'
    '            buf = b""\n'
)

CS_LOG_ANCHOR_MQTT_CLASS = (
    'def _print_verdict(sig: RelaySignature) -> None:'
)
CS_LOG_NEW_MQTT_CLASS = (
    'class MqttLogCapture:\n'
    '    """MQTT-backed CS initiator log capture.\n'
    '\n'
    '    Drop-in replacement for :class:`InitiatorLogCapture` that subscribes\n'
    '    to ``silabs/cs/raw/<locator_id>`` (published by cs_reader) instead\n'
    '    of opening the WSTK J-Link TCP socket. Lets the production\n'
    '    cs_reader.service keep owning TCP 4901 — no contention, no\n'
    '    dbgmode flips, no service stops.\n'
    '    """\n'
    '\n'
    '    def __init__(self, broker: str, port: int, locator_id: str,\n'
    '                 username: str | None = None,\n'
    '                 password: str | None = None,\n'
    '                 topic_prefix: str = "silabs/cs/raw",\n'
    '                 sink: TextIO | None = None,\n'
    '                 echo: bool = False,\n'
    '                 client_id: str | None = None):\n'
    '        import paho.mqtt.client as mqtt  # noqa: PLC0415 — keep paho optional\n'
    '        self._mqtt_mod = mqtt\n'
    '        self.broker = broker\n'
    '        self.port = int(port)\n'
    '        self.locator_id = locator_id\n'
    '        self.topic = f"{topic_prefix.rstrip(chr(47))}/{locator_id}"\n'
    '        self._sink = sink\n'
    '        self._echo = echo\n'
    '        self.events: list[LogEvent] = []\n'
    '        self._lock = threading.Lock()\n'
    '        self._connected = threading.Event()\n'
    '        self._client = mqtt.Client(\n'
    '            client_id=client_id or f"cs-log-capture-{os.getpid()}",\n'
    '            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,\n'
    '            clean_session=True,\n'
    '        )\n'
    '        if username:\n'
    '            self._client.username_pw_set(username, password)\n'
    '\n'
    '        def _on_connect(client, _userdata, _flags, _reason, _props=None):\n'
    '            client.subscribe(self.topic, qos=0)\n'
    '            self._connected.set()\n'
    '\n'
    '        def _on_message(_client, _userdata, msg):\n'
    '            try:\n'
    '                line = msg.payload.decode("utf-8", "replace")\n'
    '            except Exception:  # noqa: BLE001\n'
    '                return\n'
    '            for part in line.splitlines():\n'
    '                if part.strip():\n'
    '                    self._ingest(part)\n'
    '\n'
    '        self._client.on_connect = _on_connect\n'
    '        self._client.on_message = _on_message\n'
    '\n'
    '    def start(self) -> "MqttLogCapture":\n'
    '        self._client.connect_async(self.broker, self.port, keepalive=15)\n'
    '        self._client.loop_start()\n'
    '        return self\n'
    '\n'
    '    def stop(self) -> None:\n'
    '        try:\n'
    '            self._client.loop_stop()\n'
    '            self._client.disconnect()\n'
    '        except Exception:  # noqa: BLE001\n'
    '            pass\n'
    '\n'
    '    def __enter__(self) -> "MqttLogCapture":\n'
    '        return self.start()\n'
    '\n'
    '    def __exit__(self, *exc) -> None:\n'
    '        self.stop()\n'
    '\n'
    '    def wait_connected(self, timeout_s: float) -> bool:\n'
    '        return self._connected.wait(timeout=timeout_s)\n'
    '\n'
    '    def snapshot(self) -> list[LogEvent]:\n'
    '        with self._lock:\n'
    '            return list(self.events)\n'
    '\n'
    '    def signature(self) -> RelaySignature:\n'
    '        return summarize(self.snapshot())\n'
    '\n'
    '    def _ingest(self, line: str) -> None:\n'
    '        ev = classify(line)\n'
    '        with self._lock:\n'
    '            self.events.append(ev)\n'
    '        if self._sink is not None:\n'
    '            self._sink.write(ev.fmt() + "\\n")\n'
    '            self._sink.flush()\n'
    '        if self._echo:\n'
    '            print(ev.fmt())\n'
    '\n'
    '\n'
    'def _print_verdict(sig: RelaySignature) -> None:'
)

# Add `import os` near the top of cs_log_capture.py only if missing.
src = CS_LOG_CAPTURE.read_text()
if "\nimport os\n" not in src and not src.startswith("import os\n"):
    src2 = src.replace("import socket\n", "import os\nimport socket\n", 1)
    if src2 != src:
        backup(CS_LOG_CAPTURE)
        CS_LOG_CAPTURE.write_text(src2)
        CHANGES.append(str(CS_LOG_CAPTURE) + " [+import os]")

patch_file(
    CS_LOG_CAPTURE,
    [
        ('sock.sendall(b"GO\\n")', CS_LOG_ANCHOR_GO, CS_LOG_NEW_GO),
        ("class MqttLogCapture", CS_LOG_ANCHOR_MQTT_CLASS, CS_LOG_NEW_MQTT_CLASS),
    ],
)

# ──────────────────────────────────────────────────────────────────────────
# 4. system_test.py — Deployment.cs_log_via, MQTT broker creds, use
#    MqttLogCapture by default in the hardware-injector relay path.
# ──────────────────────────────────────────────────────────────────────────
SYSTEM_TEST_ANCHOR_IMPORT = (
    "from cs_log_capture import InitiatorLogCapture  # noqa: E402"
)
SYSTEM_TEST_NEW_IMPORT = (
    "from cs_log_capture import InitiatorLogCapture, MqttLogCapture  # noqa: E402"
)

SYSTEM_TEST_ANCHOR_FIELDS = (
    '    wstk_ip: str = "192.168.8.212"\n'
    '    locator_id: str = "ble-pd-0C4314F0319C"\n'
)
SYSTEM_TEST_NEW_FIELDS = (
    '    wstk_ip: str = "192.168.8.212"\n'
    '    locator_id: str = "ble-pd-0C4314F0319C"\n'
    '    # CS initiator log source for hardware relay tests:\n'
    '    #   "mqtt" — subscribe to silabs/cs/raw/<locator_id> (no 4901\n'
    '    #            contention; production cs_reader keeps the socket).\n'
    '    #   "tcp"  — open WSTK 4901 directly (stops cs_reader implicitly).\n'
    '    cs_log_via: str = "mqtt"\n'
    '    cs_log_topic_prefix: str = "silabs/cs/raw"\n'
    '    mqtt_username: str | None = "audit"\n'
    '    mqtt_password_env: str | None = "MQTT_AUDIT_PASSWORD"\n'
)

SYSTEM_TEST_ANCHOR_USE = (
    '    ilog: InitiatorLogCapture | None = None\n'
    '    if injector == "hardware" and scn.requires_initiator_log:\n'
    '        ilog = InitiatorLogCapture(dep.wstk_ip)\n'
    '        ilog.start()\n'
)
SYSTEM_TEST_NEW_USE = (
    '    ilog: "InitiatorLogCapture | MqttLogCapture | None" = None\n'
    '    if injector == "hardware" and scn.requires_initiator_log:\n'
    '        if dep.cs_log_via == "mqtt":\n'
    '            pw = os.environ.get(dep.mqtt_password_env or "", None)\n'
    '            ilog = MqttLogCapture(\n'
    '                broker=dep.broker,\n'
    '                port=dep.port,\n'
    '                locator_id=dep.locator_id,\n'
    '                username=dep.mqtt_username,\n'
    '                password=pw,\n'
    '                topic_prefix=dep.cs_log_topic_prefix,\n'
    '            )\n'
    '        else:\n'
    '            ilog = InitiatorLogCapture(dep.wstk_ip)\n'
    '        ilog.start()\n'
)

SYSTEM_TEST_ANCHOR_PRINT = (
    "print(f\"      {gray('capturing CS initiator log at ' + dep.wstk_ip + ':4901 for the relay signature (needs dbgmode=MCU)...')}\")"
)
SYSTEM_TEST_NEW_PRINT = (
    "if isinstance(ilog, MqttLogCapture):\n"
    "            print(f\"      {gray('capturing CS initiator log via MQTT ' + ilog.topic + ' for the relay signature (production stack untouched)...')}\")\n"
    "        else:\n"
    "            print(f\"      {gray('capturing CS initiator log at ' + dep.wstk_ip + ':4901 for the relay signature (needs dbgmode=MCU)...')}\")"
)

patch_file(
    SYSTEM_TEST,
    [
        ("MqttLogCapture", SYSTEM_TEST_ANCHOR_IMPORT, SYSTEM_TEST_NEW_IMPORT),
        ("cs_log_via", SYSTEM_TEST_ANCHOR_FIELDS, SYSTEM_TEST_NEW_FIELDS),
        ('if dep.cs_log_via ==', SYSTEM_TEST_ANCHOR_USE, SYSTEM_TEST_NEW_USE),
        ("isinstance(ilog, MqttLogCapture)", SYSTEM_TEST_ANCHOR_PRINT, SYSTEM_TEST_NEW_PRINT),
    ],
)

# ──────────────────────────────────────────────────────────────────────────
# 5. mosquitto ACL — grant cs_reader write on silabs/cs/raw/#
# ──────────────────────────────────────────────────────────────────────────
def patch_mosquitto_acl() -> bool:
    """Returns True if the ACL was changed (requires SIGHUP afterwards)."""
    try:
        src = subprocess.check_output(
            ["sudo", "-n", "cat", str(MOSQ_ACL)], text=True)
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"cannot read {MOSQ_ACL}: {e}")
    if "silabs/cs/raw" in src:
        return False
    pattern = (
        r"(user cs_reader\n"
        r"topic write silabs/cs/range/#\n"
        r"topic write silabs/cs/bond/#\n"
        r"topic write silabs/cs/status/#\n)"
    )
    m = re.search(pattern, src)
    if not m:
        raise SystemExit(f"{MOSQ_ACL}: cannot find cs_reader stanza to extend")
    new_block = m.group(1) + "topic write silabs/cs/raw/#\n"
    new_src = src[:m.start()] + new_block + src[m.end():]
    tmp = Path("/tmp/mosquitto.acl.new")
    tmp.write_text(new_src)
    subprocess.check_call(
        ["sudo", "-n", "cp", "-p", str(MOSQ_ACL), str(MOSQ_ACL) + ".bak.relay_tee"])
    subprocess.check_call(
        ["sudo", "-n", "install", "-m", "0640", "-o", "root", "-g", "mosquitto",
         str(tmp), str(MOSQ_ACL)])
    tmp.unlink(missing_ok=True)
    CHANGES.append(str(MOSQ_ACL))
    return True


acl_changed = patch_mosquitto_acl()

# ──────────────────────────────────────────────────────────────────────────
# Reload services (SIGHUP mosquitto, restart cs-reader@.service for tag IP).
# Both are minimal-blast-radius operations; no other service is touched.
# ──────────────────────────────────────────────────────────────────────────
if acl_changed:
    subprocess.run(["sudo", "-n", "systemctl", "kill", "--signal=SIGHUP", "mosquitto"],
                   check=False)
    print("  → mosquitto SIGHUP'd (ACL reload).")

if str(CS_READER) in CHANGES or str(CS_READER_CFG) in CHANGES:
    locator_id = json.loads(CS_READER_CFG.read_text())["locator_id"]
    unit = f"cs-reader@{locator_id}.service"
    subprocess.run(["sudo", "-n", "systemctl", "restart", unit], check=False)
    print(f"  → restarted {unit}")

print()
print("PATCHED:" if CHANGES else "NO CHANGES (already patched)")
for c in CHANGES:
    print("  -", c)
