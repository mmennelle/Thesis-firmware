#!/usr/bin/env bash
# Run pipeline_monitor.py ~12s and show what it prints (connection + data rows).
cd /opt/ble-thesis/testing/monitor
echo "=== audit cred present in creds.env? ==="
grep -E 'MQTT_AUDIT_PASSWORD' /opt/ble-thesis/security/mosquitto/creds.env >/dev/null && echo "  MQTT_AUDIT_PASSWORD present" || echo "  MISSING"
echo "=== run monitor 12s ==="
timeout 12 python3 pipeline_monitor.py 2>&1 | head -40
echo "=== what is live on broker right now (audit) 5s ==="
AUDITPW=$(grep -E '^MQTT_AUDIT_PASSWORD=' /opt/ble-thesis/security/mosquitto/creds.env | cut -d= -f2)
timeout 5 mosquitto_sub -h localhost -u audit -P "$AUDITPW" -t 'silabs/#' -t 'metal/#' -v 2>&1 | head -15
echo "=== done ==="
