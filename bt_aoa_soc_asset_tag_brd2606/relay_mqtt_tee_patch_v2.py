#!/usr/bin/env python3
"""Follow-up patch:

  1. /etc/mosquitto/acl — grant `bench_sim` read on silabs/cs/raw/#
     (the harness identity used by system_test.py).
  2. /opt/ble-thesis/security/cs_log_capture.py — MqttLogCapture uses
     bench_mqtt_auth.apply_mqtt_auth(client) when no explicit creds are
     passed. Same convention as every other client in system_test.py.
  3. /opt/ble-thesis/security/system_test.py — drop the per-test
     mqtt_username/mqtt_password_env Deployment fields (now redundant);
     keep cs_log_via and cs_log_topic_prefix.

Idempotent.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

CS_LOG_CAPTURE = Path("/opt/ble-thesis/security/cs_log_capture.py")
SYSTEM_TEST = Path("/opt/ble-thesis/security/system_test.py")
MOSQ_ACL = Path("/etc/mosquitto/acl")


def backup(p: Path) -> None:
    bak = p.with_suffix(p.suffix + ".bak.relay_tee_v2")
    if not bak.exists():
        shutil.copy2(p, bak)


# ──────────────────────────────────────────────────────────────────────────
# 1. ACL: add `topic read silabs/cs/raw/#` to bench_sim stanza.
# ──────────────────────────────────────────────────────────────────────────
def patch_acl() -> bool:
    src = subprocess.check_output(
        ["sudo", "-n", "cat", str(MOSQ_ACL)], text=True)
    # The bench_sim block already has a read on silabs/cs/range/+/+; add
    # silabs/cs/raw/# right after it for symmetry. Skip if already present.
    bench_block_re = re.compile(
        r"(user bench_sim\n(?:topic [a-z]+\s+[^\n]+\n)+)",
    )
    m = bench_block_re.search(src)
    if not m:
        raise SystemExit("ACL: cannot locate bench_sim stanza")
    block = m.group(1)
    if "silabs/cs/raw" in block:
        return False
    # Insert after the last 'silabs/cs/range/+/+' read line (or at end of block).
    new_line = "topic read  silabs/cs/raw/#\n"
    if "topic read  silabs/cs/range/+/+\n" in block:
        new_block = block.replace(
            "topic read  silabs/cs/range/+/+\n",
            "topic read  silabs/cs/range/+/+\n" + new_line,
            1,
        )
    else:
        new_block = block + new_line
    new_src = src[: m.start(1)] + new_block + src[m.end(1):]
    tmp = Path("/tmp/mosquitto.acl.bench")
    tmp.write_text(new_src)
    subprocess.check_call(
        ["sudo", "-n", "install", "-m", "0640", "-o", "root",
         "-g", "mosquitto", str(tmp), str(MOSQ_ACL)])
    tmp.unlink(missing_ok=True)
    return True


# ──────────────────────────────────────────────────────────────────────────
# 2. MqttLogCapture: fall back to apply_mqtt_auth when no creds passed.
# ──────────────────────────────────────────────────────────────────────────
OLD_AUTH_BLOCK = (
    '        if username:\n'
    '            self._client.username_pw_set(username, password)\n'
)
NEW_AUTH_BLOCK = (
    '        if username:\n'
    '            self._client.username_pw_set(username, password)\n'
    '        else:\n'
    '            # Fall back to the same credential resolution as every\n'
    '            # other bench-harness MQTT client (MQTT_USERNAME / \n'
    '            # MQTT_PASSWORD env vars, then security/mosquitto/creds.env).\n'
    '            try:\n'
    '                from bench_mqtt_auth import apply_mqtt_auth\n'
    '                apply_mqtt_auth(self._client)\n'
    '            except Exception:  # noqa: BLE001 — bench_mqtt_auth is optional\n'
    '                pass\n'
)

src = CS_LOG_CAPTURE.read_text()
if "bench_mqtt_auth" not in src:
    if OLD_AUTH_BLOCK not in src:
        raise SystemExit("cs_log_capture.py: cannot locate auth block")
    backup(CS_LOG_CAPTURE)
    CS_LOG_CAPTURE.write_text(src.replace(OLD_AUTH_BLOCK, NEW_AUTH_BLOCK, 1))
    print("  patched cs_log_capture.py (apply_mqtt_auth fallback)")
else:
    print("  cs_log_capture.py already wired to bench_mqtt_auth")

# ──────────────────────────────────────────────────────────────────────────
# 3. system_test.py: drop mqtt_username/mqtt_password_env from Deployment
#    and from the MqttLogCapture call site (use defaults → apply_mqtt_auth).
# ──────────────────────────────────────────────────────────────────────────
OLD_FIELDS = (
    '    cs_log_via: str = "mqtt"\n'
    '    cs_log_topic_prefix: str = "silabs/cs/raw"\n'
    '    mqtt_username: str | None = "audit"\n'
    '    mqtt_password_env: str | None = "MQTT_AUDIT_PASSWORD"\n'
)
NEW_FIELDS = (
    '    cs_log_via: str = "mqtt"\n'
    '    cs_log_topic_prefix: str = "silabs/cs/raw"\n'
)

OLD_CALL = (
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
)
NEW_CALL = (
    '        if dep.cs_log_via == "mqtt":\n'
    '            ilog = MqttLogCapture(\n'
    '                broker=dep.broker,\n'
    '                port=dep.port,\n'
    '                locator_id=dep.locator_id,\n'
    '                topic_prefix=dep.cs_log_topic_prefix,\n'
    '            )\n'
)

src = SYSTEM_TEST.read_text()
changed = False
if OLD_FIELDS in src:
    src = src.replace(OLD_FIELDS, NEW_FIELDS, 1)
    changed = True
if OLD_CALL in src:
    src = src.replace(OLD_CALL, NEW_CALL, 1)
    changed = True
if changed:
    backup(SYSTEM_TEST)
    SYSTEM_TEST.write_text(src)
    print("  patched system_test.py (dropped redundant mqtt_username/_password_env)")
else:
    print("  system_test.py already clean")

# ──────────────────────────────────────────────────────────────────────────
# Apply ACL change + SIGHUP mosquitto. Nothing else is touched.
# ──────────────────────────────────────────────────────────────────────────
if patch_acl():
    subprocess.run(
        ["sudo", "-n", "systemctl", "kill", "--signal=SIGHUP", "mosquitto"],
        check=False,
    )
    print("  ACL patched + mosquitto SIGHUP'd")
else:
    print("  ACL already grants bench_sim read on silabs/cs/raw/#")

print("done")
