"""Low-level serial diagnostics for the relay NCP on a WSTK J-Link CDC port.

Checks modem line states and tries a raw sl_bt_system_hello round-trip under a
few flow-control / DTR permutations to find one the NCP answers.
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"

# sl_bt_system_hello: BGAPI binary command.
# Header: msg_type=0x20 (command, BT), payload_len=0x00, dev/class=0x01 (system), cmd=0x00 (hello)
HELLO = bytes([0x20, 0x00, 0x01, 0x00])


def trial(rtscts, dtr, rts, label):
    try:
        s = serial.Serial()
        s.port = PORT
        s.baudrate = 115200
        s.rtscts = rtscts
        s.timeout = 1.0
        s.write_timeout = 1.0
        s.dtr = dtr
        s.rts = rts
        s.open()
    except Exception as e:
        print(f"[{label}] open failed: {e}")
        return
    try:
        try:
            s.dtr = dtr
            s.rts = rts
        except Exception:
            pass
        time.sleep(0.2)
        print(f"[{label}] lines: cts={s.cts} dsr={s.dsr} ri={s.ri} cd={s.cd}")
        try:
            s.reset_input_buffer()
            s.write(HELLO)
            s.flush()
        except Exception as e:
            print(f"[{label}] write failed (likely CTS gating): {e}")
            return
        time.sleep(0.3)
        data = s.read(64)
        print(f"[{label}] read {len(data)} bytes: {data.hex()}")
    finally:
        s.rtscts = False
        s.close()


trial(rtscts=True,  dtr=True,  rts=True,  label="rtscts+dtr")
trial(rtscts=True,  dtr=False, rts=True,  label="rtscts-no-dtr")
trial(rtscts=False, dtr=True,  rts=True,  label="noflow+dtr+rts")
trial(rtscts=False, dtr=False, rts=True,  label="noflow+rts")
