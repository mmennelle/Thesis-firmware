#!/usr/bin/env python3
# Fix: cs_reader publishes CS-piggybacked IMU to silabs/aoa/imu/<locator>/<tag>
# but the post-cutover ACL only lets it write silabs/cs/#. Add a least-privilege
# grant scoped to cs_reader's own locator_id, in BOTH the repo source-of-truth
# and the deployed ACL. Idempotent + backups.
import io, os, time, shutil

GRANT = "topic write silabs/aoa/imu/ble-pd-0C4314F0319C/#"
ANCHOR = "topic write silabs/cs/status/#"
FILES = [
    "/opt/ble-thesis/security/mosquitto/acl",  # repo source of truth
    "/etc/mosquitto/acl",                        # deployed
]

for p in FILES:
    if not os.path.exists(p):
        print("SKIP (missing):", p); continue
    src = io.open(p, encoding="utf-8").read()
    if GRANT in src:
        print("already present:", p); continue
    if ANCHOR not in src:
        print("!! anchor not found, NOT patching:", p); continue
    # Insert the grant on the line right after the cs_reader status grant.
    new = src.replace(ANCHOR + "\n", ANCHOR + "\n" + GRANT + "\n", 1)
    bak = p + ".bak." + time.strftime("%Y%m%d_%H%M%S")
    shutil.copy2(p, bak)
    io.open(p, "w", encoding="utf-8").write(new)
    print("patched:", p, "(backup", bak + ")")

print("\n== cs_reader block now ==")
import subprocess
print(io.open("/etc/mosquitto/acl", encoding="utf-8").read().split("user cs_reader",1)[1].split("user ",1)[0].strip())
