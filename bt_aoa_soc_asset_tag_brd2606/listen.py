"""Passively listen on the relay CDC port while the user presses RESET."""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

s = serial.Serial(PORT, baudrate=115200, timeout=0.3, rtscts=False)
s.dtr = True
s.rts = True
s.reset_input_buffer()
print(f"Listening on {PORT} for {SECS:.0f}s -- press the RESET button on the relay 2606 now...",
      flush=True)
buf = bytearray()
t0 = time.time()
while time.time() - t0 < SECS:
    chunk = s.read(256)
    if chunk:
        buf += chunk
        print("RX", len(chunk), chunk.hex(), flush=True)
print(f"total {len(buf)} bytes", flush=True)
s.close()
