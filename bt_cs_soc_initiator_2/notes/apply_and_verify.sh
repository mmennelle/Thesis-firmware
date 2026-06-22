#!/usr/bin/env bash
# Apply cs_reader MQTT creds to the DEPLOYED config, revert debug log, and verify
# ranges now publish (subscribe with mode_controller creds, which can read range).
set -e
IP=192.168.8.212
ts=$(date +%Y%m%d_%H%M%S)
echo "== backup + patch /etc/cs-reader/${IP}.json =="
sudo cp /etc/cs-reader/${IP}.json /etc/cs-reader/${IP}.json.bak.${ts}
sudo python3 /tmp/patch_etc_config.py
echo "== revert debug instrument in cs_reader.py =="
python3 /tmp/revert_instrument.py
cd /opt/ble-thesis/cs_reader && python3 -m py_compile cs_reader.py && echo "compiled"
echo "== prep board: stop services, dbgmode MCU, settle 7s =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 7
echo "== subscribe to ranges (mode_controller creds) in background =="
timeout 16 mosquitto_sub -h localhost -u mode_controller -P 'dZvtfjV6rolCYFRPOxXslEsbQll7PO1' \
  -t 'silabs/cs/range/#' -v > /tmp/range_capture.txt 2>/tmp/range_err.txt &
SUBPID=$!
sleep 1
echo "== run cs_reader foreground 14s (authenticated now) =="
timeout 14 python3 /opt/ble-thesis/cs_reader/cs_reader.py -c /etc/cs-reader/${IP}.json 2>&1 | head -20
wait $SUBPID 2>/dev/null || true
echo "== RANGE TOPICS CAPTURED =="
head -6 /tmp/range_capture.txt; echo "  (lines: $(wc -l < /tmp/range_capture.txt))"
echo "  sub_err: $(cat /tmp/range_err.txt)"
echo "== RESTORE =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
