#!/usr/bin/env bash
# Instrument GO send, then run cs_reader.py in FOREGROUND (no systemd) to see live output.
IP=192.168.8.212
python3 /tmp/instrument_go.py
cd /opt/ble-thesis/cs_reader
python3 -m py_compile cs_reader.py && echo "compiled"
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
echo "settle 6s"; sleep 6
echo "== run cs_reader.py foreground 12s =="
timeout 12 python3 /opt/ble-thesis/cs_reader/cs_reader.py -c /etc/cs-reader/${IP}.json 2>&1 | head -40
echo "== RESTORE =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
