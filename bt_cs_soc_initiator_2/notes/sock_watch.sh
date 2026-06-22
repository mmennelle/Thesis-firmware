#!/usr/bin/env bash
# Sample who holds the WSTK .212:4901 VCOM socket, once/sec for 100s.
# Prints timestamped rows only when a socket exists; tags the owning process.
for i in $(seq 1 100); do
  ts=$(date +%H:%M:%S)
  sudo ss -tnp 2>/dev/null | grep '192.168.8.212:4901' | sed "s|^|$ts |"
  sleep 1
done
