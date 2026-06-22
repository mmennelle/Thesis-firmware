#!/usr/bin/env bash
# Controlled CS capture to locate where the IMU-over-CS chain breaks.
# Flip .212 to MCU, GO, capture VCOM, classify lines, then restore AoA.
set -e
IP=192.168.8.212
echo "== stop AoA/controller, dbgmode MCU =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 7
echo "== GO + capture 22s =="
python3 - <<'PY'
import socket, time, re
s=socket.create_connection(("192.168.8.212",4901),timeout=5); s.settimeout(1.0)
s.sendall(b"GO\n")
buf=bytearray(); t=time.time()
while time.time()-t<22:
    try:
        d=s.recv(8192)
        if d: buf.extend(d)
    except socket.timeout: pass
try: s.sendall(b"STOP\n")
except OSError: pass
time.sleep(2)
try:
    while True:
        d=s.recv(8192)
        if not d: break
        buf.extend(d)
except OSError: pass
except socket.timeout: pass
s.close()
txt=buf.decode("latin-1", "replace")
lines=txt.splitlines()
started   = sum(1 for l in lines if "STARTED" in l)
bond      = [l for l in lines if "[BOND]" in l]
subscribe = [l for l in lines if "IMU notify: subscribing" in l]
enabled   = [l for l in lines if "IMU notify: enabled" in l]
imu       = [l for l in lines if l.startswith("[IMU]") or "[IMU]" in l[:8]]
dist      = [l for l in lines if re.search(r'distance', l, re.I)]
secfail   = [l for l in lines if re.search(r'bond|encrypt|security|insufficient|0x040[0-9a-f]', l, re.I)]
print("total lines        :", len(lines))
print("STARTED acks        :", started)
print("[BOND] lines        :", len(bond), bond[:2])
print("IMU subscribing      :", len(subscribe), subscribe[:1])
print("IMU enabled          :", len(enabled), enabled[:1])
print("[IMU] frames         :", len(imu), (imu[0][:30] if imu else ""))
print("distance-ish lines   :", len(dist))
print("security/bond errs   :", len(secfail), secfail[:4])
print("---- last 25 lines ----")
for l in lines[-25:]:
    print("  ", l[:120])
PY
echo "== restore: dbgmode OUT + start locators + controller =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" "bt-aoa-locator@192.168.8.204.service" mode-controller.service
echo done
