#!/usr/bin/env bash
# Measure IMU rate by source + bridge output for ~12s (whatever mode the FSM is in).
source /opt/ble-thesis/security/mosquitto/creds.env 2>/dev/null || true
echo "== mode =="
timeout 1 mosquitto_sub -h localhost -u audit -P "$MQTT_AUDIT_PASSWORD" -t 'metal/tool/drillpress/mode' -C 1 2>/dev/null || echo "(no retained mode)"
echo "== 12s rate by topic =="
timeout 12 mosquitto_sub -h localhost -u audit -P "$MQTT_AUDIT_PASSWORD" \
  -t 'silabs/aoa/imu/+/+' -t 'silabs/cs/range/+/+' \
  -t 'metal/tool/drillpress/+/imu/filtered' -F '%t' 2>/dev/null \
| python3 -c '
import sys,collections
c=collections.Counter()
for t in sys.stdin:
    t=t.strip()
    if not t: continue
    if t.startswith("silabs/aoa/imu/"):
        loc=t.split("/")[3]; c["imu_src:"+loc]+=1
    elif t.startswith("silabs/cs/range/"):
        c["cs_range"]+=1
    elif t.endswith("/imu/filtered"):
        c["bridge_imu_filtered"]+=1
for k,v in sorted(c.items()):
    print(f"  {k:40s} {v:4d} msgs  ~{v/12:.1f} Hz")
'
echo done
