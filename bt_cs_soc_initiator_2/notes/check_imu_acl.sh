#!/usr/bin/env bash
echo "== cs_reader ACL rules =="
sudo awk '/^user cs_reader$/{f=1;print;next} /^user /{f=0} f' /etc/mosquitto/acl
echo
echo "== cs_reader publish topic in cs_reader.py =="
grep -nE "silabs/aoa/imu|silabs/cs/|topic" /opt/ble-thesis/cs_reader/cs_reader.py | grep -iE 'imu|aoa|publish|topic =' | head -20
echo
echo "== _publish_imu topic build =="
sed -n '554,600p' /opt/ble-thesis/cs_reader/cs_reader.py
echo
echo "== mosquitto ACL denials for cs_reader (last 1h) =="
sudo journalctl -u mosquitto --since '-1 hour' 2>/dev/null | grep -iE 'denied|cs_reader|silabs/aoa' | tail -20 || echo "(mosquitto not journald or no denials)"
echo "== mosquitto log file denials =="
sudo grep -iE 'denied|ACL' /var/log/mosquitto/mosquitto.log 2>/dev/null | tail -20 || echo "(no /var/log/mosquitto denials)"
