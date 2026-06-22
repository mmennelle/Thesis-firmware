p = "/opt/ble-thesis/cs_reader/cs_reader.py"
s = open(p, encoding="utf-8").read()
old = (
'                try:\n'
'                    s.sendall(b"GO\\n")\n'
'                    self._log.info("GO kick sent on connect")\n'
'                except OSError as e:\n'
'                    self._log.warning("failed to send GO kick: %s", e)\n'
'                return\n'
)
new = (
'                try:\n'
'                    s.sendall(b"GO\\n")\n'
'                except OSError as e:\n'
'                    self._log.warning("failed to send GO kick: %s", e)\n'
'                return\n'
)
c = s.count(old)
print("count", c)
if c == 1:
    open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
    print("instrument removed")
