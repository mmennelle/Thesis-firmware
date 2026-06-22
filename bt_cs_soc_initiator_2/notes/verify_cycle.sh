#!/usr/bin/env bash
# Reload cs-reader code (restart if active) and watch one live CS cycle for ranges.
IP=192.168.8.212
echo "== restart cs-reader if active (load patched code) =="
if systemctl is-active --quiet "cs-reader@${IP}.service"; then
  sudo systemctl restart "cs-reader@${IP}.service"
  echo "  cs-reader restarted"
else
  echo "  cs-reader idle (controller will start it next CS cycle with new code)"
fi
echo "== watch controller + cs-reader for ~100s (waiting for a CS cycle) =="
timeout 100 journalctl -f -u mode-controller.service -u "cs-reader@${IP}.service" --since now -o cat \
  | grep --line-buffered -E "CS_PENDING|cs_link_up|cs_pending_timeout|-> CS|range|connected to|GO|STARTED|distance" \
  || true
echo "== done =="
