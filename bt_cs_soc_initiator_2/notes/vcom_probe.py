import socket, subprocess, time, threading

HOST, PORT = "192.168.8.212", 4901
s = socket.create_connection((HOST, PORT), timeout=5)
s.settimeout(1.0)
buf = bytearray()

def reader():
    t = time.time()
    while time.time() - t < 11:
        try:
            d = s.recv(4096)
            if d:
                buf.extend(d)
        except socket.timeout:
            pass

th = threading.Thread(target=reader)
th.start()

# 1) banner test: reset the board AFTER the socket is open
time.sleep(0.5)
subprocess.run(["commander", "device", "reset", "--ip", HOST], capture_output=True)
time.sleep(4)
mark = len(buf)

# 2) liveness test: GO must be acked with "[APP] STARTED" (direct VCOM write)
s.sendall(b"GO\n")
time.sleep(2)
s.sendall(b"STOP\n")
th.join()
try:
    s.close()
except Exception:
    pass

asc = sum(1 for b in buf if 9 <= b <= 13 or 32 <= b <= 126)
print("TOTAL BYTES:", len(buf), " ASCII-ish:", asc)
print("AFTER-RESET (banner window) first 400:", repr(bytes(buf[:mark][:400])))
print("AFTER-GO  (ack window)      first 400:", repr(bytes(buf[mark:][:400])))
print("HAS_STARTED:", b"STARTED" in buf, " HAS_BANNER:", b"CS initiator" in buf)
