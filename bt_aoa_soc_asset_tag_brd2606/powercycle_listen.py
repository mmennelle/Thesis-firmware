"""Power-cycle the target via J-Link and capture raw boot bytes on the CDC port.

A clean power-on boot is independent of any debugger halt state, so if the NCP
still emits nothing here, the cause is firmware/VCOM config (not a halted core).
"""
import subprocess
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
SERNO = sys.argv[2] if len(sys.argv) > 2 else "440358315"
COMMANDER = r"C:/Users/mpmen/.silabs/slt/installs/archive/Simplicity Commander/commander.exe"


def cmd(*args):
    r = subprocess.run([COMMANDER, *args], capture_output=True, text=True, timeout=25)
    return (r.stdout or "") + (r.stderr or "")


# Open the CDC first so we don't miss the boot burst.
s = serial.Serial(PORT, baudrate=115200, timeout=0.3, rtscts=False)
s.dtr = True
s.rts = True
s.reset_input_buffer()

print("power off:", cmd("adapter", "power", "off", "--serialno", SERNO).strip()[-120:])
time.sleep(1.0)
print("power on :", cmd("adapter", "power", "on", "--serialno", SERNO).strip()[-120:])

buf = bytearray()
t0 = time.time()
while time.time() - t0 < 4.0:
    chunk = s.read(256)
    if chunk:
        buf += chunk
print(f"captured {len(buf)} bytes: {bytes(buf).hex()}")

# Also poke a raw BGAPI hello in case it already booted silently.
s.reset_input_buffer()
s.write(bytes([0x20, 0x00, 0x01, 0x00]))
s.flush()
time.sleep(0.4)
resp = s.read(64)
print(f"hello response {len(resp)} bytes: {resp.hex()}")
s.close()
