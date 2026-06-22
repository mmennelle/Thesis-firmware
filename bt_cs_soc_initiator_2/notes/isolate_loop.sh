#!/usr/bin/env bash
# Isolate: continuous GO-read vs cs-reader-style idle-reconnect loop, settled board.
IP=192.168.8.212
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
echo "settle 8s"; sleep 8
python3 - <<PY
import socket, time
IP="${IP}"
# (1) continuous: send GO once, read 6s
s=socket.create_connection((IP,4901),timeout=5); s.settimeout(0.5)
s.sendall(b"GO\n")
buf=bytearray(); t=time.time()
while time.time()-t<6:
    try:
        d=s.recv(4096)
        if d: buf.extend(d)
    except socket.timeout: pass
s.close()
print(f"(1) continuous send-GO bytes={len(buf)} sample={bytes(buf[:90])!r}")

time.sleep(1)
# (2) cs-reader replica: idle_reconnect=3s, recv_timeout=0.5s, re-send GO on each open
def open_go():
    s=socket.create_connection((IP,4901),timeout=5); s.settimeout(0.5)
    s.sendall(b"GO\n"); return s
sock=open_go(); last=time.monotonic(); total=0; reopens=0; t0=time.time()
while time.time()-t0<12:
    try:
        d=sock.recv(4096)
        if d: total+=len(d); last=time.monotonic()
        elif not d:
            sock.close(); sock=open_go(); reopens+=1; last=time.monotonic()
    except socket.timeout:
        if time.monotonic()-last>=3.0:
            sock.close(); sock=open_go(); reopens+=1; last=time.monotonic()
sock.close()
print(f"(2) cs-reader-replica bytes={total} reopens={reopens}")
PY
echo "RESTORE"
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
