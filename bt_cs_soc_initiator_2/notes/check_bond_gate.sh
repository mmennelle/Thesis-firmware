#!/usr/bin/env bash
echo "== bond gate config (deployed) =="
grep -E 'cs_bond_check_enabled|min_cs_sec_level' /opt/ble-thesis/mode_controller/mode_controller_config.json
echo "== tools.json (deployed) =="
python3 - <<'PY'
import json
d=json.load(open('/opt/ble-thesis/mode_controller/tools.json'))
for t in d.get('tools',[]):
    print("tool=",t.get('tool_id'),"expected_addr=",t.get('expected_addr'),"authorized=",t.get('authorized_tags'))
print("tools.json mode:", oct(__import__('os').stat('/opt/ble-thesis/mode_controller/tools.json').st_mode & 0o777))
PY
echo "== any cs_bond_* reason in last 3h? =="
sudo journalctl -u mode-controller --since '-3 hours' | grep -iE 'cs_bond' | tail -8 || echo "(none seen)"
echo "== observed_addr/sec debug if logged =="
sudo journalctl -u mode-controller --since '-1 hours' | grep -iE 'observed|bond' | tail -8 || echo "(none)"
