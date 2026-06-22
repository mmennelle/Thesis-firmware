#!/usr/bin/env bash
# Reproduce the controller's CS-entry EXACTLY (no board reset): free 4901,
# dbgmode MCU, send GO, and watch whether the initiator actually ranges.
# Restores AoA at the end.
IP=192.168.8.212
echo "== stop controller + locator + cs-reader (free 4901, no reset) =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
sudo ss -tnp 2>/dev/null | grep "${IP}:4901" && echo "  WARN: 4901 still held" || echo "  4901 free"
echo "== dbgmode MCU =="
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 3
echo "== send GO (no reset) and read 15s =="
python3 - <<PY
import socket, time
s = socket.create_connection(("${IP}", 4901), timeout=5)
s.settimeout(1.0)
s.sendall(b"GO\n")
buf = bytearray(); t = time.time()
while time.time() - t < 15:
    try:
        d = s.recv(4096)
        if d: buf.extend(d)
    except socket.timeout:
        pass
s.close()
print("BYTES:", len(buf))
print("REPR:", repr(bytes(buf[:700])))
print("HAS_STARTED:", b"STARTED" in buf, " HAS_BOND:", b"[BOND]" in buf,
      " HAS_CONN:", b"Connection opened" in buf, " HAS_DIST:", (b"distance" in buf or b"Obj" in buf))
PY
echo "== RESTORE =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo "== done =="
