#!/usr/bin/env bash
# Real patched cs-reader service, but settle 9s after dbgmode (board boot).
IP=192.168.8.212
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service" "cs-reader@${IP}.service"
sleep 2
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
echo "settle 9s"; sleep 9
sudo systemctl start "cs-reader@${IP}.service"
sleep 12
echo "== cs-reader log =="
journalctl -u "cs-reader@${IP}.service" --since '-14 sec' -o cat --no-pager | tail -22
echo "RESTORE"
sudo systemctl stop "cs-reader@${IP}.service"; sleep 1
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo done
