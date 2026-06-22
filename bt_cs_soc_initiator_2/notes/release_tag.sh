#!/usr/bin/env bash
# My manual GO left the CS initiator connected to the tag. Send STOP to release it
# so the tag resumes AoA CTE, then verify angle/position flow in AOA mode.
set -e
IP=192.168.8.212
AUDITPW=$(grep -E '^MQTT_AUDIT_PASSWORD=' /opt/ble-thesis/security/mosquitto/creds.env | cut -d= -f2)
echo "== stop controller+locator, dbgmode MCU, send STOP to release tag =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 5
python3 - <<PY
import socket, time
s=socket.create_connection(("${IP}",4901),timeout=5); s.settimeout(1.0)
s.sendall(b"STOP\n")
buf=bytearray(); t=time.time()
while time.time()-t<4:
    try:
        d=s.recv(4096)
        if d: buf.extend(d)
    except socket.timeout: pass
s.close()
print("  STOP ack STOPPED seen:", b"STOPPED" in buf, " sample:", bytes(buf[:120]))
PY
echo "== dbgmode OUT, restart locator + controller =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" "bt-aoa-locator@192.168.8.204.service" mode-controller.service
echo "== wait 8s for AoA to establish, then check angle + position (12s) =="
sleep 8
timeout 12 mosquitto_sub -h localhost -u audit -P "$AUDITPW" \
  -t 'silabs/aoa/angle/#' -t 'metal/tool/+/+/position/filtered' -v 2>&1 | head -10
echo "  (above: angle + position; empty = still no CTE)"
echo done
