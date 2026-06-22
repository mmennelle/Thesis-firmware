#!/usr/bin/env python3
import io, time, shutil
P = "/opt/ble-thesis/bridge/docs/SECURITY_PIPELINE.md"
src = io.open(P, encoding="utf-8").read()

old_row = "| `cs_reader` | `silabs/cs/range/#`, `silabs/cs/bond/#`, `silabs/cs/status/#` | — |"
new_row = "| `cs_reader` | `silabs/cs/range/#`, `silabs/cs/bond/#`, `silabs/cs/status/#`, `silabs/aoa/imu/ble-pd-0C4314F0319C/#` | — |"

old_lead = "Two ACL facts worth recording for the writeup:"
new_lead = "Three ACL facts worth recording for the writeup:"

old_bullet = (
"* The new **`silabs/cs/bond/#`** topic (for §2) was added to the ACL:\n"
"  `cs_reader` write, `mode_controller` read.\n"
)
new_bullet = old_bullet + (
"* `cs_reader` also writes **`silabs/aoa/imu/ble-pd-0C4314F0319C/#`** (its\n"
"  own locator subtree). The CS path piggybacks the tag's IMU over the CS\n"
"  GATT connection at ~10 Hz and `cs_reader` republishes it under the AoA\n"
"  IMU topic so the bridge ingests it identically to advertising-sourced\n"
"  IMU (`PIPELINE.md` §5.4 dual-transport). This grant was **missing in the\n"
"  initial cutover ACL**, which silently capped IMU at the ~3 Hz\n"
"  advertising rate during CS (the second AoA anchor's adverts) instead of\n"
"  ~10 Hz — a concrete instance of the silent-ACL-denial risk: mosquitto\n"
"  does not log per-message publish denials by default. Restored; verified\n"
"  ~9.9 Hz `imu/filtered` during a live CS episode.\n"
)

assert old_row in src, "row not found"
assert old_lead in src, "lead not found"
assert old_bullet in src, "bullet not found"

src = src.replace(old_row, new_row, 1).replace(old_lead, new_lead, 1).replace(old_bullet, new_bullet, 1)

bak = P + ".bak." + time.strftime("%Y%m%d_%H%M%S")
shutil.copy2(P, bak)
io.open(P, "w", encoding="utf-8").write(src)
print("patched", P, "backup", bak)
