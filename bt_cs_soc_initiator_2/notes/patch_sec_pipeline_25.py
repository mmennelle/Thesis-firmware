#!/usr/bin/env python3
# Patch SECURITY_PIPELINE.md section 2.5 to reflect the verified current
# firmware state (replaces the stale "silent VCOM / needs reflash" caveat).
import io, os, time, shutil

P = "/opt/ble-thesis/bridge/docs/SECURITY_PIPELINE.md"
src = io.open(P, encoding="utf-8").read()

start_marker = "### 2.5 Operational caveat"
end_marker = "## 3. The MQTT control plane"

i = src.index(start_marker)
j = src.index(end_marker)

new_section = (
"### 2.5 Current state (verified 2026-06-21)\n"
"\n"
"The gate is **live, enabled, and fails closed**\n"
"(`cs_bond_check_enabled: true`, `min_cs_sec_level: 2` in the deployed\n"
"`mode_controller_config.json`), and the production tag\n"
"`ble-pd-449FDA247AD0` is pinned in `tools.json`\n"
"(`expected_addr: 449FDA247AD0`; file mode `0644`, public address only).\n"
"\n"
"The initiator firmware on `.212` (`bt_cs_soc_initiator_2`) is **healthy\n"
"and verified**. Its security build is plain Secure-Connections JustWorks:\n"
"`CS_APP_CAPABILITY = NoInputNoOutput`, `PREFER_AUTENTICATED_PAIRING = 0`\n"
"and `NEW_BOND_REQUIRES_PASSKEY = 0` (so neither `MITM_REQUIRED` nor\n"
"`CONNECTIONS_FROM_BONDED_DEVICES_ONLY` is set), with only\n"
"`REJECT_DEBUG_KEYS` enabled. Once the link is encrypted the firmware\n"
"raises SC to **security level 2** and emits `[BOND] 449FDA247AD0 2` once\n"
"per CS episode (`CS_INIT_FORWARD_BOND = 1`), exactly as §2.2 describes.\n"
"The stronger IRK-forwarding path is present in the source but compiled\n"
"out (`CS_INIT_FORWARD_IRK = 0`; it also needs the external-bonding-database\n"
"component, which is not in the project) — see §9.\n"
"\n"
"End-to-end CS authorization now **passes**. Live episodes transition\n"
"`CS_PENDING -> CS (cs_link_up)` and leave CS only on operational reasons\n"
"(`cs_motion`, `cs_distance_oo_zone`); across the verification window there\n"
"were **zero** `cs_bond_missing` / `cs_bond_addr_mismatch` denials, i.e.\n"
"the forwarded address and security level satisfy the gate every episode.\n"
"A representative authenticated CS session published distances around\n"
"3.7 m at ~7.4 Hz with a monotonic sequence and no reconnect loop.\n"
"\n"
"> **Narrative note (supersedes the earlier \"silent VCOM\" diagnosis).**\n"
"> A prior revision of this section recorded the initiator as silent on\n"
"> VCOM and concluded the firmware needed reflashing. That was a\n"
"> misdiagnosis worth keeping for the writeup, because the root cause is a\n"
"> non-obvious property of the test harness: the WSTK J-Link VCOM is\n"
"> **write-gated** — a TCP reader receives the UART->TCP stream only after\n"
"> it has itself written at least one byte (the `GO\\n` gate command). The\n"
"> reader (`cs_reader.py`) was purely passive and so observed nothing,\n"
"> while the firmware was in fact ranging normally. The fix was entirely\n"
"> host-side (`cs_reader` now sends `GO\\n` on every connect; plus the\n"
"> MQTT-auth credentials of §3) — **no firmware change was required**. The\n"
"> initiator firmware is unchanged and correct.\n"
"\n"
"---\n"
"\n"
)

out = src[:i] + new_section + src[j:]

bak = P + ".bak." + time.strftime("%Y%m%d_%H%M%S")
shutil.copy2(P, bak)
io.open(P, "w", encoding="utf-8").write(out)
print("patched:", P)
print("backup :", bak)
print("section 2.5 now starts:")
k = out.index("### 2.5")
print(out[k:k+90])
