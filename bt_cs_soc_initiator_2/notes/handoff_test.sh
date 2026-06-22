#!/usr/bin/env bash
# Reproduce the EXACT live handoff: transient GO socket (like the controller),
# then hand 4901 to the real cs-reader and see if it pulls ranges.
IP=192.168.8.212
echo "== free 4901 (stop controller/locator/cs-reader), dbgmode MCU =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 3
echo "== controller-style transient GO (connect, GO, read STARTED, close) =="
python3 - <<PY
import socket, time
s = socket.create_connection(("${IP}", 4901), timeout=5)
s.settimeout(1.0)
s.sendall(b"GO\n")
t=time.time(); got=b""
while time.time()-t < 4:
    try:
        d=s.recv(4096)
        if d: got+=d
        if b"STARTED" in got: break
    except socket.timeout: pass
s.close()
print("  GO ack seen:", b"STARTED" in got)
PY
echo "== now start the REAL cs-reader on the (already-ranging) initiator =="
sudo systemctl start "cs-reader@${IP}.service"
sleep 10
echo "== cs-reader log =="
journalctl -u "cs-reader@${IP}.service" --since '-12 sec' --no-pager | tail -15
echo "== RESTORE =="
sudo systemctl stop "cs-reader@${IP}.service"
sleep 1
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo "== done =="
