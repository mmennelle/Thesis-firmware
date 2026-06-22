#!/usr/bin/env bash
# Apply the GO-kick patch to cs_reader.py (write-gated VCOM fix). Backs up first.
set -e
cd /opt/ble-thesis/cs_reader
ts=$(date +%Y%m%d_%H%M%S)
cp cs_reader.py "cs_reader.py.bak.${ts}"
echo "backup: cs_reader.py.bak.${ts}"
python3 - <<'PY'
import io
p = "cs_reader.py"
src = open(p, encoding="utf-8").read()
old = (
'                self._sock = s\n'
'                self._log.info("connected to %s:%d", self.host, self.port)\n'
'                return\n'
)
new = (
'                self._sock = s\n'
'                self._log.info("connected to %s:%d", self.host, self.port)\n'
'                # WSTK J-Link VCOM only streams UART->TCP to a client that has\n'
'                # written at least one byte. cs-reader is otherwise passive, so\n'
'                # kick the stream with GO on every (re)connect. GO is idempotent\n'
'                # in the initiator firmware (re-confirms CS gate, harmless if\n'
'                # already ranging).\n'
'                try:\n'
'                    s.sendall(b"GO\\n")\n'
'                except OSError as e:\n'
'                    self._log.warning("failed to send GO kick: %s", e)\n'
'                return\n'
)
n = src.count(old)
if n != 1:
    raise SystemExit(f"ERROR: expected exactly 1 match, found {n}; aborting")
open(p, "w", encoding="utf-8").write(src.replace(old, new, 1))
print("patched OK")
PY
echo "== verify syntax =="
python3 -m py_compile cs_reader.py && echo "py_compile OK"
echo "== context around _open =="
sed -n '206,232p' cs_reader.py
