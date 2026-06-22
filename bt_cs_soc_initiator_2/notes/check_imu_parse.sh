#!/usr/bin/env bash
echo "== cs_reader.py IMU parsing present? =="
grep -nE 'IMU_RE|_handle_imu_frame|_publish_imu|\[IMU\]|IMU_MAGIC|def _handle_line' /opt/ble-thesis/cs_reader/cs_reader.py || echo "!! NO IMU PARSING IN cs_reader.py !!"
echo
echo "== GO-kick present? =="
grep -nE 'GO kick|sendall\(b"GO' /opt/ble-thesis/cs_reader/cs_reader.py || echo "(no GO-kick)"
echo
echo "== backups available =="
ls -la /opt/ble-thesis/cs_reader/cs_reader.py* 2>/dev/null
echo
echo "== mtime of deployed cs_reader.py =="
stat -c '%y  %s bytes' /opt/ble-thesis/cs_reader/cs_reader.py
