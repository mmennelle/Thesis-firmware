"""Pull the genuine CS initiator's app-log over its WSTK TCP VCOM (192.168.8.212:4901).

The WSTK VCOM is write-gated and single-client: a reader must send >=1 byte (here the
firmware's `GO\n` gate command) or it receives nothing. Stop the Pi services holding 4901
(cs-reader@.212 / bt-aoa-locator@.212 / mode-controller) before running, or this socket
will block out / get no data.

Usage:
    python capture_initiator_log.py [--host 192.168.8.212] [--port 4901] [--seconds 25] [--go]
"""
import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.212")
    ap.add_argument("--port", type=int, default=4901)
    ap.add_argument("--seconds", type=float, default=25.0)
    ap.add_argument("--go", action="store_true",
                    help="send 'GO\\n' to un-gate the firmware (default: just 'GO\\n').")
    ap.add_argument("--no-go", dest="go", action="store_false")
    ap.add_argument("--strings", action="store_true",
                    help="extract only printable-ASCII runs (>=4 chars) to dig the app_log "
                         "out of the interleaved binary RTL/CS trace.")
    ap.add_argument("--minlen", type=int, default=4)
    ap.set_defaults(go=True)
    args = ap.parse_args()

    print(f"connecting to {args.host}:{args.port} ...", flush=True)
    s = socket.create_connection((args.host, args.port), timeout=5)
    s.settimeout(0.5)
    if args.go:
        s.sendall(b"GO\n")  # un-gate + satisfy the write-gated WSTK VCOM
        print("sent GO", flush=True)

    deadline = time.time() + args.seconds
    buf = b""
    try:
        while time.time() < deadline:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                print("(socket closed by peer)", flush=True)
                break
            buf += chunk
            if args.strings:
                # Pull maximal runs of printable ASCII (incl. space/tab) out of the
                # binary trace; emit runs >= minlen so app_log text survives.
                run = bytearray()
                rest = bytearray()
                for b in buf:
                    if 0x20 <= b <= 0x7E or b in (0x09,):
                        run.append(b)
                    else:
                        if len(run) >= args.minlen:
                            print(f"{time.strftime('%H:%M:%S')}  {run.decode('ascii')}",
                                  flush=True)
                        run = bytearray()
                # keep a possibly-incomplete trailing run for next chunk
                buf = bytes(run)
                _ = rest
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    print(f"{time.strftime('%H:%M:%S')}  {text}", flush=True)
    finally:
        try:
            s.close()
        except OSError:
            pass
    if buf:
        print(f"{time.strftime('%H:%M:%S')}  {buf.decode('utf-8', 'replace')!r}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
