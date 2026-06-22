#!/usr/bin/env python3
import io, time, shutil

P = "/opt/ble-thesis/bridge/docs/SECURITY_PIPELINE.md"
src = io.open(P, encoding="utf-8").read()
edits = []

# --- Edit 1: §3.2 cs_reader IMU rate bullet -> current state ---
old1 = """* `cs_reader` also writes **`silabs/aoa/imu/ble-pd-0C4314F0319C/#`** (its
  own locator subtree). The CS path piggybacks the tag's IMU over the CS
  GATT connection at ~10 Hz and `cs_reader` republishes it under the AoA
  IMU topic so the bridge ingests it identically to advertising-sourced
  IMU (`PIPELINE.md` §5.4 dual-transport). This grant was **missing in the
  initial cutover ACL**, which silently capped IMU at the ~3 Hz
  advertising rate during CS (the second AoA anchor's adverts) instead of
  ~10 Hz — a concrete instance of the silent-ACL-denial risk: mosquitto
  does not log per-message publish denials by default. Restored; verified
  ~9.9 Hz `imu/filtered` during a live CS episode."""
new1 = """* `cs_reader` also writes **`silabs/aoa/imu/ble-pd-0C4314F0319C/#`** (its
  own locator subtree). The CS path piggybacks the tag's IMU over the CS
  GATT connection and `cs_reader` republishes it under the AoA IMU topic
  so the bridge ingests it identically to advertising-sourced IMU
  (`PIPELINE.md` §5.4 dual-transport). The tag firmware notifies one IMU
  sample per fused reading — the sequence number advances exactly once per
  sample, so the bridge's seq-dedup neither collapses nor double-counts —
  and the CS-path stream runs at the fusion rate (~50 Hz), with headroom
  on the 7.5 ms CS connection interval. The AoA advertising path (used
  when no CS link is up) runs at 10 Hz. This ACL grant was **missing in
  the initial cutover ACL**, which silently dropped the CS-path IMU
  publishes entirely (leaving only the second AoA anchor's adverts) — a
  concrete instance of the silent-ACL-denial risk: mosquitto does not log
  per-message publish denials by default. The grant is in place and the
  CS-path IMU publishes are accepted."""
edits.append((old1, new1))

# --- Edit 2: §6 intro rate framing ---
old2 = """**Reviewer issue (rate-invariance, `sentinel-opt.txt`).** The detector
was empirically fit at **~3 Hz** filtered IMU. The IMU-over-CS change
opens the post-auth window exactly when the tag notifies at **~10 Hz**
(dropping back to ~3 Hz if CS disconnects mid-window). Several gates are
expressed in **per-sample** units and silently change meaning at the new
rate:"""
new2 = """**Reviewer issue (rate-invariance, `sentinel-opt.txt`).** The detector
was empirically fit at **~3 Hz** filtered IMU. The IMU-over-CS transport
opens the post-auth window exactly when the IMU stream is at its fastest:
the tag notifies **once per fused sample** over the CS GATT link, so the
post-auth window sees IMU at **up to ~50 Hz** (the fusion rate), dropping
back to the **10 Hz** AoA advertising path if CS disconnects mid-window.
The fit baseline (~3 Hz) is now an order of magnitude below the operating
rate. Several gates are expressed in **per-sample** units and silently
change meaning at the higher rate:"""
edits.append((old2, new2))

# --- Edit 3: §6 item #1 ---
old3 = """   at 10 Hz they collapse to ~0.33 s / ~2 s, contaminating the gravity
   estimate that the `handled` discriminator depends on. **Fix:** drive"""
new3 = """   at 10 Hz they collapse to ~0.33 s / ~2 s, and over the CS link (tens of
   Hz) to well under that, contaminating the gravity estimate that the
   `handled` discriminator depends on. **Fix:** drive"""
edits.append((old3, new3))

# --- Edit 4: §6 item #2 ---
old4 = """2. `sustained_violation_count = 2` samples ≈ 0.67 s @ 3 Hz but 0.2 s
   @ 10 Hz → risk of false `erratic` on ordinary walking. **Fix:**"""
new4 = """2. `sustained_violation_count = 2` samples ≈ 0.67 s @ 3 Hz but 0.2 s
   @ 10 Hz, and only ~0.04 s at the CS-path rate → escalating risk of
   false `erratic` on ordinary motion as the rate rises. **Fix:**"""
edits.append((old4, new4))

# --- Edit 5: §6 item #4 ---
old5 = """4. Upstream: the bridge filters IMU at a **fixed** `imu_filter_sampling_hz`
   (`aoa_bridge.py`), so amplitude thresholds fit at 3 Hz may not hold at
   10 Hz — capture a `profile3.jsonl` during a live CS session and re-run
   the replay harness to re-validate."""
new5 = """4. Upstream: the bridge filters IMU at a **fixed** `imu_filter_sampling_hz`
   (`aoa_bridge.py`), so amplitude thresholds fit at 3 Hz may not hold at
   the much higher CS-path rate — capture a `profile.jsonl` during a live
   CS session at the current rate and re-run the replay harness to
   re-validate."""
edits.append((old5, new5))

# --- Edit 6: §6 status note ---
old6 = """**Status: OPEN — documented, not yet implemented.** #1 and #2 are
correctness regressions introduced by the rate change; #3 is robustness;
#4 is the threshold re-fit."""
new6 = """**Status: OPEN — documented, not yet implemented.** #1 and #2 are
correctness regressions widened by the higher CS-path IMU rate (now up to
~50 Hz, vs. the ~3 Hz fit); #3 is robustness; #4 is the threshold re-fit."""
edits.append((old6, new6))

# --- Edit 7: §9 §8.12.12 fingerprint advert cadence ---
old7 = """* **Resolvable Private Address on the tag (§8.12.12).** Static address +
  20 ms CTE cadence + the `IM` manufacturer advert are a long-term
  tracking fingerprint."""
new7 = """* **Resolvable Private Address on the tag (§8.12.12).** Static address +
  20 ms CTE cadence + the 10 Hz `IM` manufacturer advert are a long-term
  tracking fingerprint."""
edits.append((old7, new7))

# --- Edit 8: §10.1 sentinel row ---
old8 = "| sentinel | IMU rate-invariance | n/a | OPEN — documented (§6) |"
new8 = "| sentinel | IMU rate-invariance | n/a | OPEN — documented (§6); operating IMU now 10 Hz (AoA adv) / up to ~50 Hz (CS link) vs. ~3 Hz fit |"
edits.append((old8, new8))

for i, (o, n) in enumerate(edits, 1):
    assert src.count(o) == 1, f"edit {i}: expected exactly 1 match, found {src.count(o)}"
    src = src.replace(o, n, 1)

# --- Edit 9: append Appendix A change log (idempotent) ---
APPENDIX = """

---

## Appendix A. Change log — 2026-06-22 tag firmware IMU rate update

The tag firmware (`bt_aoa_soc_asset_tag_brd2606`) IMU transport was
adjusted so the IMU payload is phase-locked to the fusion sample (the
sequence number advances exactly once per fused reading) and both
transports run at their maximum useful rate: the AoA advertising path at
**10 Hz** and the CS GATT-notify path at the **fusion rate (~50 Hz)**.
The radio-layer authorization controls are unaffected — the address-pin
bond gate (§2), MQTT auth + ACLs (§3), and CS distance gate (§4) do not
depend on the IMU cadence. The only security-relevant consequence is the
IMU rate observed by the post-auth motion sentinel (§6). Sections updated:

| Doc location | What changed |
|--------------|--------------|
| §3.2 (ACL bullet — `cs_reader` IMU grant) | CS-path IMU now described as fusion-rate (~50 Hz, one sequence number per fused sample) with headroom on the 7.5 ms CS connection interval; AoA advert path stated as 10 Hz. Prior text cited a fixed ~10 Hz / "~9.9 Hz verified" against the old pinned cadence. |
| §6 (motion-sentinel rate-invariance) | Post-auth IMU rate updated from ~10 Hz (drop to ~3 Hz) to up to ~50 Hz over CS (drop to 10 Hz over AoA adv); per-sample examples (#1, #2) and the status note extended to the higher rate, widening the ~3 Hz-fit vs. operating-rate gap. |
| §9 (§8.12.12 fingerprint) | `IM` manufacturer advert cadence noted as 10 Hz (was 3 Hz). |
| §10.1 (status table, sentinel row) | Annotated current operating IMU rates (10 Hz AoA / up to ~50 Hz CS) vs. the ~3 Hz fit. |
"""
if "## Appendix A. Change log — 2026-06-22" not in src:
    src = src.rstrip() + "\n" + APPENDIX

bak = P + ".bak." + time.strftime("%Y%m%d_%H%M%S")
shutil.copy2(P, bak)
io.open(P, "w", encoding="utf-8").write(src)
print("patched", P)
print("backup", bak)
