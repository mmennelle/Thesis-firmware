"""Probe each J-Link CDC port to find which one runs a BGAPI NCP target.

An NCP answers sl_bt_system_hello with a response; SoC/app firmware does not.
"""
import sys
import bgapi

API_XAPI = [
    r"C:/Users/mpmen/.silabs/slt/installs/conan/p/simpl35774a752829c/p/bluetooth_le_host/api/sl_bt.xapi",
]
PORTS = sys.argv[1:] or ["COM4", "COM6"]
# DK2606A board controller keeps VCOM handshake disabled, so rtscts=False works best
# (RTS asserted by pyserial holds the EFR's CTS active even though firmware has HW flow ctrl).
RTSCTS = False

for port in PORTS:
    try:
        lib = bgapi.BGLib(bgapi.SerialConnector(port, baudrate=115200, rtscts=RTSCTS), API_XAPI)
        lib.open()
        try:
            lib.bt.system.hello()  # response command
            v = lib.bt.system.get_version()
            print(f"{port}: NCP OK -> stack {v.major}.{v.minor}.{v.patch} build {v.build}")
        finally:
            lib.close()
    except Exception as e:
        print(f"{port}: not an NCP / no response ({type(e).__name__}: {e})")
