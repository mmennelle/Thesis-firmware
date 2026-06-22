#!/usr/bin/env bash
echo "=== host_locator source dir ==="
d=$(sudo find /opt/ble-thesis /home /usr/local -type d -iname '*host_locator*' 2>/dev/null | head -3); echo "$d"
echo "=== grep 'allow' meaning in host_locator source ==="
sudo grep -rnE 'allow[^a-zA-Z]|allowlist|allow_|"allow"' $(sudo find /opt/ble-thesis /home -type d -iname '*locator*' 2>/dev/null) 2>/dev/null | grep -iE 'allow' | grep -iE 'leg_adv|allow=|filter|cte|connect|bond|auth|irk' | head -20
echo "=== locator config / allowlist (tag) ==="
sudo find /etc /opt/ble-thesis -iname '*locator*' \( -name '*.json' -o -name '*.conf' -o -name '*.env' \) 2>/dev/null | head
echo "=== does locator config reference the tag / IRK / bonding? ==="
sudo grep -rinE '449FDA247AD0|44:9F:DA:24:7A:D0|irk|bond|allow' /etc/bt-aoa-locator* /etc/default/bt-aoa-locator* 2>/dev/null | head -15
echo done
