"""Open the CDC port, reset the target via J-Link, and capture raw boot bytes.

A live bt_cs_ncp emits a sl_bt_evt_system_boot the instant it resets. If we see
ANY bytes, the UART link works and we can decode them; if nothing at 115200, we
retry a few common baud rates.
"""
import subprocess
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
SERNO = sys.argv[2] if len(sys.argv) > 2 else "440358315"
COMMANDER = r"C:/Users/mpmen/.silabs/slt/installs/archive/Simplicity Commander/commander.exe"

for baud in (115200, 921600, 460800, 230400, 57600):
    try:
        s = serial.Serial(PORT, baudrate=baud, timeout=0.3, rtscts=True)
    except Exception as e:
        print(f"[{baud}] open failed: {e}")
        continue
    try:
        s.dtr = True
        s.rts = True
        s.reset_input_buffer()
        # Reset the chip so it re-emits the boot event.
        subprocess.run([COMMANDER, "device", "reset", "--serialno", SERNO],
                       capture_output=True, timeout=20)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 2.0:
            chunk = s.read(256)
            if chunk:
                buf += chunk
        print(f"[{baud}] captured {len(buf)} bytes: {bytes(buf).hex()}")
        if buf:
            print(f"  -> link alive at {baud}")
            break
    finally:
        s.rtscts = False
        s.close()
