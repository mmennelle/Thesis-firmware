#!/usr/bin/env bash
AUDITPW=$(grep -E '^MQTT_AUDIT_PASSWORD=' /opt/ble-thesis/security/mosquitto/creds.env | cut -d= -f2)
echo "=== silabs/aoa/angle/# (6s) ==="
timeout 6 mosquitto_sub -h localhost -u audit -P "$AUDITPW" -t 'silabs/aoa/angle/#' -v 2>&1 | head -8
echo "  (angle lines above; empty = no angle being published)"
echo "=== position/filtered (6s) ==="
timeout 6 mosquitto_sub -h localhost -u audit -P "$AUDITPW" -t 'metal/tool/+/+/position/filtered' -v 2>&1 | head -5
echo "=== bt-aoa-locator services ==="
systemctl list-units 'bt-aoa-locator@*' --no-legend 2>/dev/null
echo "=== bt-aoa-locator@192.168.8.212 recent log ==="
journalctl -u 'bt-aoa-locator@192.168.8.212.service' --since '-90 sec' -o cat --no-pager | tail -20
echo "=== aoa-bridge recent log ==="
journalctl -u 'aoa-bridge.service' --since '-90 sec' -o cat --no-pager 2>/dev/null | tail -12
echo done
