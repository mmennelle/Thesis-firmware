#!/usr/bin/env bash
f=$(sudo find /opt/ble-thesis /home /usr/local -iname 'pipeline_monitor.py' 2>/dev/null | head -1)
echo "FILE: $f"
echo "=== mqtt usage ==="
sudo grep -nE 'username|password|pw_set|connect\(|Client\(|subscribe|broker|1883|config|argparse|add_argument' "$f" | head -40
