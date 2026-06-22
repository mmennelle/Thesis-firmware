#!/usr/bin/env bash
# Decisive: on an already-ranging initiator, compare a SILENT reader socket
# vs a socket that (re)sends GO. Isolates whether the reader must write GO.
IP=192.168.8.212
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 3
python3 - <<PY
import socket, time
def read_only(label, send_go, secs):
    s = socket.create_connection(("${IP}", 4901), timeout=5)
    s.settimeout(1.0)
    if send_go:
        s.sendall(b"GO\n")
    buf=bytearray(); t=time.time()
    while time.time()-t < secs:
        try:
            d=s.recv(4096)
            if d: buf.extend(d)
        except socket.timeout: pass
    s.close()
    print(f"  [{label}] send_go={send_go} bytes={len(buf)} sample={bytes(buf[:80])!r}")
    return len(buf)

# A: prime the firmware into ranging with a GO socket, then close it.
read_only("A-prime-GO", True, 4)
time.sleep(0.5)
# B: SILENT reader (the cs-reader case) on already-ranging firmware.
read_only("B-silent", False, 5)
time.sleep(0.5)
# C: fresh socket that RE-sends GO.
read_only("C-resend-GO", True, 5)
PY
echo "== RESTORE =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
