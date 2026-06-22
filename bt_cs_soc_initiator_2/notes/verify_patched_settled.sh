#!/usr/bin/env bash
# Post-patch check: settle after dbgmode, then let the REAL patched cs-reader
# (which now sends GO on connect) pull ranges. No controller GO involved.
IP=192.168.8.212
echo "== free 4901, dbgmode MCU, SETTLE 4s =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sleep 4
echo "== start patched cs-reader, watch 14s =="
sudo systemctl start "cs-reader@${IP}.service"
sleep 14
echo "== cs-reader log =="
journalctl -u "cs-reader@${IP}.service" --since '-16 sec' -o cat --no-pager | tail -25
echo "== any range topics published? (mosquitto_sub 3s) =="
timeout 3 mosquitto_sub -h localhost -t 'silabs/cs/range/#' -v 2>/dev/null | head -5 || echo "  (no auth/none captured)"
echo "== RESTORE =="
sudo systemctl stop "cs-reader@${IP}.service"; sleep 1
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
