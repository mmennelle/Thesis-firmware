#!/usr/bin/env python3
"""Patch /etc/cs-reader/192.168.8.212.json (the real per-instance config)
to enable the raw-VCOM-line MQTT tee."""
import json, pathlib, sys
p = pathlib.Path('/etc/cs-reader/192.168.8.212.json')
cfg = json.loads(p.read_text())
changed = False
if 'raw_tee_enabled' not in cfg:
    cfg['raw_tee_enabled'] = True
    changed = True
if 'raw_tee_topic_prefix' not in cfg:
    cfg['raw_tee_topic_prefix'] = 'silabs/cs/raw'
    changed = True
if changed:
    bak = p.with_suffix('.json.bak.relay_tee')
    if not bak.exists():
        bak.write_text(p.read_text())
    p.write_text(json.dumps(cfg, indent=2) + '\n')
    print('patched')
else:
    print('already')
