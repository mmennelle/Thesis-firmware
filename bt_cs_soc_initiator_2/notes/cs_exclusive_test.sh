#!/usr/bin/env bash
# Validate root cause: with bt-aoa-locator stopped (4901 freed), does the CS
# path actually stream through cs-reader? Restores everything at the end.
set -e
IP=192.168.8.212
echo "== stop controller + locator (free 4901) =="
sudo systemctl stop mode-controller.service "bt-aoa-locator@${IP}.service"
sleep 1
echo "== confirm 4901 free =="
sudo ss -tnp 2>/dev/null | grep "${IP}:4901" || echo "  (no client on 4901 - good)"
echo "== dbgmode MCU + start cs-reader =="
/usr/local/bin/commander adapter dbgmode MCU --ip ${IP} >/dev/null 2>&1
sudo systemctl start "cs-reader@${IP}.service"
sleep 18
echo "== cs-reader log (look for ranges/publish, not just 'connected') =="
journalctl -u "cs-reader@${IP}.service" --since '-20 sec' --no-pager | tail -25
echo "== who holds 4901 now =="
sudo ss -tnp 2>/dev/null | grep "${IP}:4901" || echo "  (none)"
echo "== RESTORE =="
sudo systemctl stop "cs-reader@${IP}.service"
sleep 1
/usr/local/bin/commander adapter dbgmode OUT --ip ${IP} >/dev/null 2>&1
sudo systemctl start "bt-aoa-locator@${IP}.service" mode-controller.service
echo "== done =="
