#!/usr/bin/env bash
# Reload broker, then prove cs_reader may now write silabs/aoa/imu.
set -e
source /opt/ble-thesis/security/mosquitto/creds.env 2>/dev/null || true
CSPW="A0CcljzbSZ4rOfku2WOvEXT37kdRj10v"
echo "== reload mosquitto (SIGHUP reloads acl/passwd) =="
sudo systemctl reload mosquitto || sudo systemctl restart mosquitto
sleep 1
echo "== audit subscriber (background, 4s) =="
timeout 4 mosquitto_sub -h localhost -u audit -P "$MQTT_AUDIT_PASSWORD" \
  -t 'silabs/aoa/imu/ble-pd-0C4314F0319C/#' -v > /tmp/imu_acl_test.txt 2>&1 &
SUBPID=$!
sleep 1
echo "== publish a test IMU msg AS cs_reader =="
mosquitto_pub -h localhost -u cs_reader -P "$CSPW" \
  -t 'silabs/aoa/imu/ble-pd-0C4314F0319C/ble-pd-449FDA247AD0' \
  -m '{"acl_probe":true}' ; echo "  pub exit=$?"
wait $SUBPID 2>/dev/null || true
echo "== audit received: =="
cat /tmp/imu_acl_test.txt
if grep -q acl_probe /tmp/imu_acl_test.txt; then
  echo ">> PASS: cs_reader -> silabs/aoa/imu now ALLOWED"
else
  echo ">> FAIL: still blocked (check ACL/reload)"
fi
