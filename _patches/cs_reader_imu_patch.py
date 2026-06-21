#!/usr/bin/env python3
"""Idempotent patcher: add IMU-piggyback parsing/publishing to cs_reader.py.

Run on the host:  python3 cs_reader_imu_patch.py /opt/ble-thesis/cs_reader/cs_reader.py
Writes a .bak the first time, then applies the edits. Each replacement asserts
its anchor is present exactly once so a drifted file fails loudly instead of
silently mis-patching.
"""
import sys
import pathlib

target = pathlib.Path(sys.argv[1])
src = target.read_text()

if "IMU_RE = re.compile" in src:
    print("already patched; nothing to do")
    sys.exit(0)

def repl(text, old, new):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"anchor not unique (found {n}x):\n{old[:120]!r}")
    return text.replace(old, new)

# 1) import struct
src = repl(
    src,
    "import socket\nimport sys\n",
    "import socket\nimport struct\nimport sys\n",
)

# 2) IMU regex + constants after STORM_RE
src = repl(
    src,
    'STORM_RE = re.compile(\n'
    '    r"Maximum number of initiator instances|"\n'
    '    r"Failed to create initiator instance, error:0x1c"\n'
    ')\n',
    'STORM_RE = re.compile(\n'
    '    r"Maximum number of initiator instances|"\n'
    '    r"Failed to create initiator instance, error:0x1c"\n'
    ')\n\n'
    '# IMU piggyback frame forwarded by the CS initiator over VCOM:\n'
    '#   "[IMU] " + 48 hex chars == the tag\'s raw 24-byte IMU payload.\n'
    '# The tag streams this over the CS connection (GATT notification) because\n'
    '# its standalone IMU advertising set is starved to ~2-3 Hz while connected.\n'
    '# 24-byte layout (matches the adv manufacturer payload after company id):\n'
    '#   magic(2 LE 0x494D) ver(1) seq(1) accel_xyz_mg(int16x3 LE)\n'
    '#   gyro_xyz_dps(int16x3 LE) quat_xyzw_x10000(int16x4 LE).\n'
    'IMU_RE = re.compile(r"^\\[IMU\\]\\s+([0-9A-Fa-f]{48})\\s*$")\n'
    'IMU_MAGIC = 0x494D\n'
    'IMU_VER = 1\n',
)

# 3) per-instance last-MAC cache
src = repl(
    src,
    "        self._stop = Event()\n"
    "        self._seq: dict[str, int] = {}\n",
    "        self._stop = Event()\n"
    "        self._seq: dict[str, int] = {}\n"
    "        # Most recent CS-measurement MAC; used to attribute MAC-less IMU\n"
    "        # frames (single connection, IMU subscribed only after first CS result).\n"
    "        self._last_mac: str | None = None\n",
)

# 4) handle IMU lines first in dispatch
src = repl(
    src,
    "    def _handle_line(self, line: str) -> None:\n"
    "        if not line:\n"
    "            return\n"
    "        # Drop the periodic header line cheaply before regex.\n",
    "    def _handle_line(self, line: str) -> None:\n"
    "        if not line:\n"
    "            return\n"
    "        # IMU piggyback frame (highest-rate line during CS) -- handle first.\n"
    "        im = IMU_RE.match(line)\n"
    "        if im is not None:\n"
    "            self._handle_imu_frame(im.group(1))\n"
    "            return\n"
    "        # Drop the periodic header line cheaply before regex.\n",
)

# 5) cache MAC on each CS measurement
src = repl(
    src,
    "        tag_id = mac_to_tag_id(mac)\n"
    "        seq = self._seq.get(tag_id, 0) + 1\n",
    "        tag_id = mac_to_tag_id(mac)\n"
    "        self._last_mac = mac\n"
    "        seq = self._seq.get(tag_id, 0) + 1\n",
)

# 6) new IMU decode + publish methods (inserted before the Config/CLI divider)
src = repl(
    src,
    "        self._client.publish(\n"
    "            topic, json.dumps(payload), qos=self._mqtt_qos, retain=self._mqtt_retain\n"
    "        )\n"
    "\n"
    "\n"
    "# \u2500\u2500 Config / CLI",
    "        self._client.publish(\n"
    "            topic, json.dumps(payload), qos=self._mqtt_qos, retain=self._mqtt_retain\n"
    "        )\n"
    "\n"
    "    # \u2500\u2500 IMU piggyback \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"
    "\n"
    "    def _handle_imu_frame(self, hexstr: str) -> None:\n"
    '        """Decode a 24-byte IMU frame forwarded by the initiator and publish it.\n'
    "\n"
    "        The frame carries no MAC, so it is attributed to the tag of the most\n"
    "        recent CS measurement (CS_INITIATOR_MAX_CONNECTIONS == 1, and the\n"
    "        initiator subscribes to IMU only after the first CS result, so a MAC\n"
    "        is always known by the time IMU frames arrive).\n"
    '        """\n'
    "        if self._last_mac is None:\n"
    '            self._log.debug("IMU frame before any CS measurement; dropping")\n'
    "            return\n"
    "        try:\n"
    "            data = bytes.fromhex(hexstr)\n"
    "        except ValueError:\n"
    "            return\n"
    "        if len(data) != 24:\n"
    "            return\n"
    "        (magic, ver, seq,\n"
    "         ax, ay, az,\n"
    "         gx, gy, gz,\n"
    "         qx, qy, qz, qw) = struct.unpack(\"<HBBhhhhhhhhhh\", data)\n"
    "        if magic != IMU_MAGIC or ver != IMU_VER:\n"
    '            self._log.debug("IMU frame bad magic/ver: %#06x/%d", magic, ver)\n'
    "            return\n"
    "        self._publish_imu(\n"
    "            mac=self._last_mac, seq=seq,\n"
    "            ax_mg=ax, ay_mg=ay, az_mg=az,\n"
    "            gx_dps=gx, gy_dps=gy, gz_dps=gz,\n"
    "            qx=qx / 10000.0, qy=qy / 10000.0, qz=qz / 10000.0, qw=qw / 10000.0,\n"
    "        )\n"
    "\n"
    "    def _publish_imu(\n"
    "        self, mac: str, seq: int,\n"
    "        ax_mg: int, ay_mg: int, az_mg: int,\n"
    "        gx_dps: int, gy_dps: int, gz_dps: int,\n"
    "        qx: float, qy: float, qz: float, qw: float,\n"
    "    ) -> None:\n"
    '        """Publish a decoded IMU sample on the AoA IMU topic.\n'
    "\n"
    "        Uses the exact topic and JSON schema that ``bt_aoa_host_locator``\n"
    "        emits (``silabs/aoa/imu/{locator_id}/{tag_id}``) so ``aoa_bridge``\n"
    "        ingests CS-piggybacked IMU identically to advertising-sourced IMU.\n"
    "        The tag's own uint8 ``seq`` is forwarded verbatim so the bridge's\n"
    "        per-tag de-duplication collapses the slower advertising copy and the\n"
    "        faster notification copy of the same sample into one.\n"
    '        """\n'
    "        tag_id = mac_to_tag_id(mac)\n"
    '        topic = f"silabs/aoa/imu/{self._locator_id}/{tag_id}"\n'
    "        payload = {\n"
    '            "tag": tag_id,\n'
    '            "locator": self._locator_id,\n'
    '            "seq": int(seq) & 0xFF,\n'
    '            "accel_x_mg": int(ax_mg),\n'
    '            "accel_y_mg": int(ay_mg),\n'
    '            "accel_z_mg": int(az_mg),\n'
    '            "gyro_x_dps": int(gx_dps),\n'
    '            "gyro_y_dps": int(gy_dps),\n'
    '            "gyro_z_dps": int(gz_dps),\n'
    '            "quat_x": round(qx, 4),\n'
    '            "quat_y": round(qy, 4),\n'
    '            "quat_z": round(qz, 4),\n'
    '            "quat_w": round(qw, 4),\n'
    "        }\n"
    "        self._client.publish(\n"
    "            topic, json.dumps(payload), qos=self._mqtt_qos, retain=self._mqtt_retain\n"
    "        )\n"
    "\n"
    "\n"
    "# \u2500\u2500 Config / CLI",
)

bak = target.with_suffix(target.suffix + ".bak")
if not bak.exists():
    bak.write_text(target.read_text())
    print(f"backup written: {bak}")
target.write_text(src)
print("patched OK")
