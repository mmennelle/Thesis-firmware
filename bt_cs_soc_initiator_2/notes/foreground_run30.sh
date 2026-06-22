#!/usr/bin/env bash
# Re-run cs_reader.py foreground for ~30s so the WSTK LCD error message can be read.
IP=192.168.8.212
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
echo "settle 6s"; sleep 6
echo "== run cs_reader.py foreground 30s (READ THE WSTK LCD NOW) =="
timeout 30 python3 /opt/ble-thesis/cs_reader/cs_reader.py -c /etc/cs-reader/${IP}.json 2>&1 | head -60
echo "== RESTORE =="
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
