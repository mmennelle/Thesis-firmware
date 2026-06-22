/***************************************************************************//**
 * @file
 * @brief CS Initiator example application logic
 *******************************************************************************
 * # License
 * <b>Copyright 2025 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
// -----------------------------------------------------------------------------
// Includes
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "sl_bluetooth.h"
#include "sl_component_catalog.h"
#include "app_assert.h"
#include "sl_rtl_clib_api.h"

// app content
#include "sl_main_init.h"
#include "app.h"
#include "trace.h"
#include "app_config.h"
#include "app_timer.h"

// initiator content
#include "cs_antenna.h"
#include "cs_result.h"
#include "cs_result_config.h"
#include "cs_initiator.h"
#include "cs_initiator_client.h"
#include "cs_initiator_config.h"
#include "cs_initiator_display_core.h"
#include "cs_initiator_display.h"
#include "cs_sync_antenna.h"

// RAS
#include "cs_ras_client.h"

// other required content
#include "ble_peer_manager_common.h"
#include "ble_peer_manager_connections.h"
#include "ble_peer_manager_central.h"
#include "ble_peer_manager_filter.h"

#ifdef SL_CATALOG_CS_INITIATOR_CLI_PRESENT
#include "cs_initiator_cli.h"
#endif // SL_CATALOG_CS_INITIATOR_CLI_PRESENT

#ifdef SL_CATALOG_SIMPLE_BUTTON_PRESENT
#include "sl_simple_button.h"
#include "sl_simple_button_instances.h"
#endif // SL_CATALOG_SIMPLE_BUTTON_PRESENT

// Security
#include "security.h"

// VCOM command gate I/O (AoA <-> CS arbitration, host-driven)
#include <string.h>
#include "sl_iostream.h"
#include "sl_iostream_handles.h"

// -----------------------------------------------------------------------------
// Macros

#define MAX_PERCENTAGE                   100u
#define NL                               APP_LOG_NL
#define APP_PREFIX                       "[APP] "
#define INSTANCE_PREFIX                  "[%u] "
#define APP_INSTANCE_PREFIX              APP_PREFIX INSTANCE_PREFIX
#define BT_ADDR_LEN                      sizeof(bd_addr)
#define DISPLAY_REFRESH_RATE             1000u // ms
#define ABS(x)                           ((x < 0) ? ((-1) * x) : x)
// Subfeature bitmask for CS Channel Selection Algorithm #3c
#define CS_CHANNEL_SELECTION_ALGORITHM_3C_SUBFEATURE_BITMASK 2
// Subfeature bitmask for CS Role Initiator
#define CS_ROLE_INITIATOR_SUBFEATURE_BITMASK 0
// Subfeature bitmask for CS Role Reflector
#define CS_ROLE_REFLECTOR_SUBFEATURE_BITMASK 1

// -----------------------------------------------------------------------------
// Enums, structs, typedef

// Measurement structure
typedef struct {
  float distance_filtered;
  float distance_raw;
  float likeliness;
  float distance_estimate_rssi;
  float velocity;
  float bit_error_rate;
} cs_measurement_data_t;

// CS initiator instance
typedef struct {
  uint8_t conn_handle;
  uint32_t measurement_cnt;
  uint32_t ranging_counter;
  cs_measurement_data_t measurement_mainmode;
  cs_measurement_data_t measurement_submode;
  cs_intermediate_result_t measurement_progress;
  bool measurement_arrived;
  bool measurement_progress_changed;
  bool read_remote_capabilities;
  bool security_increased;
  uint8_t number_of_measurements;
} cs_initiator_instances_t;

// -----------------------------------------------------------------------------
// Static function declarations

static uint8_t get_algo_mode(void);
static const char *antenna_usage_to_str(const cs_initiator_config_t *config);
static const char *algo_mode_to_str(uint8_t algo_mode);
static void cs_on_result(const uint8_t conn_handle,
                         const uint16_t ranging_counter,
                         const uint8_t *result,
                         const cs_result_session_data_t *result_data,
                         const cs_ranging_data_t *ranging_data,
                         const void *user_data);
static void cs_on_intermediate_result(const cs_intermediate_result_t *intermediate_result,
                                      const void *user_data);
static void cs_on_error(uint8_t conn_handle,
                        cs_error_event_t err_evt,
                        sl_status_t sc);
static sl_status_t get_instance_number(uint8_t conn_handle, uint8_t *instance_num);
static sl_status_t save_connection(uint8_t conn_handle);
static void check_cli_values(void);
static sl_status_t create_new_initiator_instance(uint8_t conn_handle);
static void delete_initiator_instance(uint8_t conn_handle);
static void app_timer_callback(app_timer_t *timer, void *data);
static void check_supported_capabilities(const sl_bt_msg_t *evt);
static void print_head_and_data(cs_initiator_instances_t *initiator);

// -----------------------------------------------------------------------------
// Static variables

static bool antenna_set_pbr = false;
static bool antenna_set_rtt = false;
static cs_initiator_config_t initiator_config = INITIATOR_CONFIG_DEFAULT;
static rtl_config_t rtl_config = RTL_CONFIG_DEFAULT;
static cs_initiator_instances_t cs_initiator_instances[CS_INITIATOR_MAX_CONNECTIONS];

// -----------------------------------------------------------------------------
// IMU piggyback (GATT client)
//
// The asset tag exposes a custom IMU Service whose "IMU Data" characteristic
// streams the fused 24-byte inertial sample over the connection via
// notifications. While the tag is connected for CS its standalone IMU
// advertising set is starved to ~2-3 Hz, so we subscribe to that notification
// here and forward each frame to the host over VCOM, where it is published at
// the full connected rate (~10 Hz).
//
// IMU_DATA_CHAR_HANDLE is the tag's gattdb_imu_data value handle (see
// bt_aoa_soc_asset_tag_brd2606/autogen/gatt_db.h). Both firmwares are built
// from the same GATT layout so the handle is stable; if the tag's GATT
// database changes, update this constant to the regenerated value.
// sl_bt_gatt_set_characteristic_notification() locates the characteristic's
// client-config descriptor itself, so no manual service/char discovery is
// needed.
#define IMU_DATA_CHAR_HANDLE   27u
#define IMU_FRAME_LEN          24u

typedef enum {
  IMU_SUB_IDLE = 0,  // not subscribed yet (waiting for CS to settle)
  IMU_SUB_PENDING,   // CCCD write issued, awaiting procedure completion
  IMU_SUB_DONE       // notifications enabled
} imu_sub_state_t;

static uint8_t imu_conn = SL_BT_INVALID_CONNECTION_HANDLE;
static imu_sub_state_t imu_sub_state = IMU_SUB_IDLE;
static bool imu_sub_arm = false; // set once CS is producing results

// Forward one IMU frame to the host as a single distinct, easily-parsed line:
// "[IMU] " + uppercase hex + "\n". The "[IMU] " prefix keeps it clear of the
// host cs_reader's distance-row and gate-ack ("[APP] ...") patterns.
static void imu_forward_frame(const uint8_t *data, size_t len)
{
  static const char hexd[] = "0123456789ABCDEF";
  char line[6 + (IMU_FRAME_LEN * 2) + 1];
  size_t o = 0u;
  line[o++] = '[';
  line[o++] = 'I';
  line[o++] = 'M';
  line[o++] = 'U';
  line[o++] = ']';
  line[o++] = ' ';
  for (size_t i = 0u; (i < len) && (i < IMU_FRAME_LEN); i++) {
    line[o++] = hexd[(data[i] >> 4) & 0x0Fu];
    line[o++] = hexd[data[i] & 0x0Fu];
  }
  line[o++] = '\n';
  (void)sl_iostream_write(sl_iostream_vcom_handle, line, o);
}

// ---------------------------------------------------------------------------
// CS authentication Layer 2 (address pin): peer-identity forward (default ON)
// ---------------------------------------------------------------------------
// The initiator already raises security on every connection (see the
// connection_parameters handler). Once the link is encrypted we forward the
// connected tag's Bluetooth address and the achieved security level to the
// host as a single, easily-parsed line:
//   "[BOND] <ADDR12> <SEC>\n"
//     ADDR12 : 12 uppercase hex, MSB-first (matches the host tag id, e.g.
//              ble-pd-449FDA247AD0 -> 449FDA247AD0)
//     SEC    : security level 1..4 (sl_bt_connection_security_t + 1)
// The host mode_controller pins ADDR (and a minimum SEC) per CS episode. This
// is emitted well after connection + encryption, never during the idle VCOM
// GO/STOP gate window, so it cannot disturb the CS handshake. Unlike the raw
// IRK, the address is available without the external bonding database, so no
// component swap / project regeneration is required.
#ifndef CS_INIT_FORWARD_BOND
#define CS_INIT_FORWARD_BOND 1
#endif

#if CS_INIT_FORWARD_BOND
static void bond_forward(uint8_t connection, uint8_t security_mode)
{
  static const char hexd[] = "0123456789ABCDEF";
  static const char pfx[] = "[BOND] ";
  bd_addr *addr = ble_peer_manager_get_bt_address(connection);
  if (addr == NULL) {
    return;
  }
  char line[sizeof(pfx) + 12 + 1 + 1]; // prefix + 12 hex + ' ' + sec + '\n'
  size_t o = 0u;
  for (size_t i = 0u; i < (sizeof(pfx) - 1u); i++) {
    line[o++] = pfx[i];
  }
  for (int i = 5; i >= 0; i--) {
    line[o++] = hexd[(addr->addr[i] >> 4) & 0x0Fu];
    line[o++] = hexd[addr->addr[i] & 0x0Fu];
  }
  line[o++] = ' ';
  line[o++] = (char)('0' + (uint8_t)((security_mode + 1u) & 0x0Fu)); // 1..4
  line[o++] = '\n';
  (void)sl_iostream_write(sl_iostream_vcom_handle, line, o);
}
#endif // CS_INIT_FORWARD_BOND

// ---------------------------------------------------------------------------
// CS authentication Layer 2: peer-IRK capture + forward (default OFF)
// ---------------------------------------------------------------------------
// The asset tag distributes its Identity Resolving Key (IRK) during Secure
// Connections bonding (the initiator already raises security on every
// connection -- see the connection_parameters handler). The raw IRK bytes are
// only visible to the application through the External Bonding Database feature
// (bluetooth_feature_external_bonding_database): sl_bt_sm_get_bonding_details
// exposes address/level/key-size but NOT the IRK. All of the code below is
// therefore compiled only when that component is present; until it is added to
// the project the catalog symbol is undefined and this is a no-op, so the
// CS / AoA / IMU pipeline is byte-for-byte unchanged.
//
// CS_INIT_FORWARD_IRK additionally gates the host-facing emission, so that even
// with the component present nothing new appears on VCOM unless explicitly
// enabled for a security-enrolled build.
#ifndef CS_INIT_FORWARD_IRK
#define CS_INIT_FORWARD_IRK 0
#endif

#if defined(SL_CATALOG_BLUETOOTH_FEATURE_EXTERNAL_BONDING_DATABASE_PRESENT)
// Provide a local IRK to the stack on request. We do NOT persist bondings
// (every connection re-pairs, which is the same code path as a first connect),
// so a per-boot random local identity is sufficient and is generated lazily.
static void ebd_get_local_irk(uint8_t out[16])
{
  static bool have_irk = false;
  static uint8_t local_irk[16];
  if (!have_irk) {
    size_t got = 0u;
    sl_status_t sc = sl_bt_system_get_random_data(sizeof(local_irk),
                                                  sizeof(local_irk),
                                                  &got, local_irk);
    if ((sc != SL_STATUS_OK) || (got < sizeof(local_irk))) {
      // Deterministic fallback: always hand the stack a valid 16-byte key.
      for (size_t i = 0u; i < sizeof(local_irk); i++) {
        local_irk[i] = (uint8_t)(0xA5u ^ i);
      }
    }
    have_irk = true;
  }
  memcpy(out, local_irk, 16u);
}

#if CS_INIT_FORWARD_IRK
// Forward the connected tag's IRK to the host as one easily-parsed line:
// "[IRK] " + uppercase hex + "\n" (bytes in the little-endian order delivered
// by the stack). The host mode_controller pins this exact value per CS episode
// (on_cs_peer_irk / _cs_irk_ok); enrollment records the same forwarded value,
// so the byte order only has to be self-consistent.
static void irk_forward(const uint8_t *irk, size_t len)
{
  static const char hexd[] = "0123456789ABCDEF";
  char line[6 + (16 * 2) + 1];
  size_t o = 0u;
  line[o++] = '[';
  line[o++] = 'I';
  line[o++] = 'R';
  line[o++] = 'K';
  line[o++] = ']';
  line[o++] = ' ';
  for (size_t i = 0u; (i < len) && (i < 16u); i++) {
    line[o++] = hexd[(irk[i] >> 4) & 0x0Fu];
    line[o++] = hexd[irk[i] & 0x0Fu];
  }
  line[o++] = '\n';
  (void)sl_iostream_write(sl_iostream_vcom_handle, line, o);
}
#endif // CS_INIT_FORWARD_IRK
#endif // SL_CATALOG_BLUETOOTH_FEATURE_EXTERNAL_BONDING_DATABASE_PRESENT

// Derive the active connection count from the instance table so the value
// can never get out of sync with the actual array contents (prevents stale
// counter leaks that pinned creation at SL_STATUS_FULL / 0x1c).
static uint8_t count_active_reflector_connections(void)
{
  uint8_t n = 0u;
  for (uint8_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    if (cs_initiator_instances[i].conn_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
      n++;
    }
  }
  return n;
}
static app_timer_t display_timer;
static uint8_t measurement_counter = 0u;

// -----------------------------------------------------------------------------
// VCOM command gate (AoA <-> CS arbitration)
//
// The combined AoA+CS asset tag has a single radio and can only run ONE
// modality at a time. The host-side mode_controller decides which: in AoA
// mode the tag must stay UNCONNECTED so its CTE tone stream runs; for CS mode
// this initiator connects and ranges. The initiator therefore stays IDLE
// until the host explicitly enables it over VCOM:
//
//   host -> initiator : "GO\n"   start scanning + connect + range
//                       "STOP\n" stop ranging, close connection, go idle
//   initiator -> host : a line containing "STARTED" (for GO) or "STOPPED"
//                       (for STOP), '\n'-terminated. Acked so the host can
//                       retry on timeout — VCOM is not lossless. GO is acked on
//                       "scanning enabled" (fits the host's 3 s budget); STOP
//                       is acked only AFTER the link is actually down so the
//                       host's dbgmode->OUT flip can't race a live CS link.
//
// Boot default is STOP: if the initiator resets mid-session it comes up idle
// and never auto-reconnects, so it can no longer silently pin the tag in CS
// mode (the original always-on-connect failure mode).
static bool cs_gate_enabled = false;
static bool gate_stop_ack_pending = false; // STOPPED deferred until conn closes
static char gate_cmd[16];
static uint8_t gate_cmd_len = 0u;

static void gate_send(const char *msg)
{
  (void)sl_iostream_write(sl_iostream_vcom_handle, msg, strlen(msg));
}

// Start scanning for a reflector ONLY when the host has enabled CS. Every
// (re)scan site funnels through here so a STOP can never be undone by an
// automatic reconnect.
static sl_status_t gated_start_scanning(void)
{
  if (!cs_gate_enabled) {
    return SL_STATUS_OK; // idle: stay disconnected so the tag runs AoA
  }
  sl_status_t sc = ble_peer_manager_central_create_connection();
  if (sc == SL_STATUS_OK) {
    cs_initiator_display_start_scanning();
  }
  return sc;
}

static void gate_close_all(void)
{
  for (uint8_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    uint8_t h = cs_initiator_instances[i].conn_handle;
    if (h != SL_BT_INVALID_CONNECTION_HANDLE) {
      (void)ble_peer_manager_central_close_connection(h);
    }
  }
}

// Fully reset the peer-manager central state so a later GO can cleanly restart
// scanning. Calling sl_bt_scanner_stop() directly stops the hardware scanner
// but leaves the peer manager's internal `scanning` flag stuck true, which
// makes the next create_connection() a silent no-op ("Already scanning") so the
// initiator never reconnects. init() clears that state but also wipes the scan
// filter, so we re-apply the boot filter (name + RAS service UUID) here.
static void gate_reset_peer_manager(void)
{
  ble_peer_manager_central_init();
  ble_peer_manager_filter_init();
  (void)ble_peer_manager_set_filter_device_name(REFLECTOR_DEVICE_NAME,
                                                strlen(REFLECTOR_DEVICE_NAME),
                                                false);
  uint16_t ras_service_uuid = CS_RAS_SERVICE_UUID;
  (void)ble_peer_manager_set_filter_service_uuid16((sl_bt_uuid_16_t *)&ras_service_uuid);
}

// GO: enable scanning/initiating, then ack on "scanning enabled" (NOT full
// connect — BLE connect + CS setup can exceed the host's 3 s timeout).
static void gate_go(void)
{
  if (!cs_gate_enabled) {
    cs_gate_enabled = true;
    gate_stop_ack_pending = false; // a fresh GO cancels a pending STOP ack
    (void)gated_start_scanning();
    log_info(APP_PREFIX "CS gate -> GO (CS mode)" NL);
  }
  gate_send("[APP] STARTED\n");
}

// STOP: stop scanning + drop the link, go idle. Ack only once the connection
// is actually closed (deferred to the conn_closed handler); if already idle,
// ack immediately so the host's idempotent retries still succeed.
static void gate_stop(void)
{
  bool was_enabled = cs_gate_enabled;
  cs_gate_enabled = false;
  if (was_enabled) {
    log_info(APP_PREFIX "CS gate -> STOP (AoA mode)" NL);
  }
  if (count_active_reflector_connections() > 0u) {
    gate_stop_ack_pending = true;
    gate_close_all(); // STOPPED is sent from conn_closed once the link drops
  } else {
    // Not connected (idle or mid-scan): reset the peer manager so the hardware
    // scanner stops AND the internal `scanning` flag clears. A bare
    // sl_bt_scanner_stop() here would wedge the next GO into a silent no-op.
    gate_reset_peer_manager();
    gate_stop_ack_pending = false;
    gate_send("[APP] STOPPED\n");
  }
}

static void gate_handle_line(void)
{
  // Trim leading/trailing whitespace and match the WHOLE trimmed line so log
  // noise can never trigger a command.
  uint8_t s = 0u;
  while (s < gate_cmd_len && (gate_cmd[s] == ' ' || gate_cmd[s] == '\t')) {
    s++;
  }
  uint8_t e = gate_cmd_len;
  while (e > s && (gate_cmd[e - 1u] == ' ' || gate_cmd[e - 1u] == '\t')) {
    e--;
  }
  gate_cmd[e] = '\0';
  const char *line = (const char *)&gate_cmd[s];
  if (*line == '\0') {
    return;
  }
  if (strcmp(line, "STOP") == 0) {
    gate_stop();
  } else if (strcmp(line, "GO") == 0) {
    gate_go();
  }
  // Unknown line: ignore silently.
}

// Poll VCOM for framed line commands. Called every app_process_action tick.
static void gate_poll_vcom(void)
{
  uint8_t buf[16];
  size_t n = 0u;
  if (sl_iostream_read(sl_iostream_vcom_handle, buf, sizeof(buf), &n) != SL_STATUS_OK) {
    return;
  }
  for (size_t i = 0u; i < n; i++) {
    char c = (char)buf[i];
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      gate_handle_line();
      gate_cmd_len = 0u;
    } else if (gate_cmd_len < sizeof(gate_cmd) - 1u) {
      gate_cmd[gate_cmd_len++] = c;
    } else {
      gate_cmd_len = 0u; // overflow: resync on next newline
    }
  }
}

/******************************************************************************
 * Application Init
 *****************************************************************************/
void app_init(void)
{
  sl_status_t sc = SL_STATUS_OK;

  trace_init();

  // initialize initiator instances
  for (uint32_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    cs_initiator_instances[i].conn_handle = SL_BT_INVALID_CONNECTION_HANDLE;
    cs_initiator_instances[i].measurement_cnt = 0u;
    cs_initiator_instances[i].ranging_counter = 0u;
    memset(&cs_initiator_instances[i].measurement_mainmode, 0u, sizeof(cs_measurement_data_t));
    memset(&cs_initiator_instances[i].measurement_submode, 0u, sizeof(cs_measurement_data_t));
    memset(&cs_initiator_instances[i].measurement_progress, 0u, sizeof(cs_intermediate_result_t));
    cs_initiator_instances[i].measurement_arrived = false;
    cs_initiator_instances[i].measurement_progress_changed = false;
    cs_initiator_instances[i].read_remote_capabilities = false;
    cs_initiator_instances[i].security_increased = false;
    cs_initiator_instances[i].number_of_measurements = 0u;
  }
  security_set_config_flags();

  // Set configuration parameters
  rtl_config.algo_mode = get_algo_mode();
  cs_initiator_apply_channel_map_preset(initiator_config.channel_map_preset,
                                        initiator_config.channel_map.data);

  if ((initiator_config.cs_main_mode == sl_bt_cs_mode_pbr)
      && (initiator_config.cs_sub_mode == sl_bt_cs_mode_rtt)) {
    // Currently, only main mode = pbr and submode = rtt is supported
    initiator_config.channel_map_preset = CS_CHANNEL_MAP_PRESET_HIGH;
    app_log_info(APP_PREFIX "Channel map preset set to high" APP_LOG_NL);
  }

  // Log configuration parameters
  log_info("+-[CS initiator by Silicon Labs]--------------------------+" NL);
  log_info("+---------------------------------------------------------+" NL);
  if (initiator_config.procedure_scheduling != CS_PROCEDURE_SCHEDULING_CUSTOM) {
    log_info(APP_PREFIX "Using %s based procedure scheduling." NL,
             initiator_config.procedure_scheduling == CS_PROCEDURE_SCHEDULING_OPTIMIZED_FOR_FREQUENCY
             ? "frequency update" : "energy consumption");
  } else {
    log_info(APP_PREFIX "Using custom procedure scheduling." NL);
  }
  log_info(APP_PREFIX "%s" NL,
           (initiator_config.max_procedure_count == 0) ? "Free running." : "Start new procedure after one finished.");
  log_info(APP_PREFIX "Antenna offset: wire%s" NL,
           CS_INITIATOR_ANTENNA_OFFSET ? "d" : "less");
  log_info(APP_PREFIX "Default CS procedure interval: %u" NL, initiator_config.min_procedure_interval);
  log_info(APP_PREFIX "CS main mode: %s (%u)" NL,
           (initiator_config.cs_main_mode == sl_bt_cs_mode_pbr) ? "PBR" : "RTT",
           initiator_config.cs_main_mode);
  log_info(APP_PREFIX "CS sub mode: %s (%u)" NL,
           (initiator_config.cs_sub_mode == sl_bt_cs_submode_disabled) ? "Disabled" : "RTT",
           initiator_config.cs_sub_mode);
  log_info(APP_PREFIX "Requested antenna usage: %s" NL, antenna_usage_to_str(&initiator_config));
  log_info(APP_PREFIX "Object tracking mode: %s" NL, algo_mode_to_str(rtl_config.algo_mode));
  log_info(APP_PREFIX "CS channel map preset: %d" NL, initiator_config.channel_map_preset);
  log_info(APP_PREFIX "CS channel map: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X" NL,
           initiator_config.channel_map.data[0],
           initiator_config.channel_map.data[1],
           initiator_config.channel_map.data[2],
           initiator_config.channel_map.data[3],
           initiator_config.channel_map.data[4],
           initiator_config.channel_map.data[5],
           initiator_config.channel_map.data[6],
           initiator_config.channel_map.data[7],
           initiator_config.channel_map.data[8],
           initiator_config.channel_map.data[9]);
  log_info(APP_PREFIX "RSSI reference TX power @ 1m: %d dBm" NL,
           (int)initiator_config.rssi_ref_tx_power);
  log_info("+-------------------------------------------------------+" NL);

  sc = cs_initiator_display_init();
  app_assert_status_f(sc, "cs_initiator_display_init failed");
  cs_initiator_display_set_measurement_mode(initiator_config.cs_main_mode, rtl_config.algo_mode);
  app_timer_start(&display_timer, DISPLAY_REFRESH_RATE, app_timer_callback, NULL, true);

  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////
}

/******************************************************************************
 * Application Process Action
 *****************************************************************************/
void app_process_action(void)
{
  gate_poll_vcom();
  sl_status_t sc = security_send_confirmation();
  if (sc != SL_STATUS_OK) {
    log_error(APP_PREFIX "Failed to send security confirmation: 0x%04lx" NL, (unsigned long)sc);
  }
  for (uint8_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    if (cs_initiator_instances[i].measurement_arrived) {
      cs_initiator_instances[i].measurement_arrived = false;
      print_head_and_data(&cs_initiator_instances[i]);
      cs_initiator_display_update_data(i,
                                       cs_initiator_instances[i].conn_handle,
                                       CS_INITIATOR_DISPLAY_STATUS_CONNECTED,
                                       cs_initiator_instances[i].measurement_mainmode.distance_filtered,
                                       cs_initiator_instances[i].measurement_mainmode.distance_estimate_rssi,
                                       cs_initiator_instances[i].measurement_mainmode.likeliness,
                                       cs_initiator_instances[i].measurement_mainmode.bit_error_rate,
                                       cs_initiator_instances[i].measurement_mainmode.distance_raw,
                                       cs_initiator_instances[i].measurement_progress.progress_percentage,
                                       rtl_config.algo_mode,
                                       initiator_config.cs_main_mode);
      measurement_counter++;
    } else if (cs_initiator_instances[i].measurement_progress_changed) {
      // write measurement progress to the display without changing the last valid
      // measurement results
      cs_initiator_instances[i].measurement_progress_changed = false;
      log_info(APP_INSTANCE_PREFIX "# %04lu ---" NL,
               cs_initiator_instances[i].measurement_progress.connection,
               cs_initiator_instances[i].measurement_cnt);

      log_info(APP_INSTANCE_PREFIX "Estimation in progress: %3u.%02u %%" NL,
               cs_initiator_instances[i].measurement_progress.connection,
               ((uint8_t)cs_initiator_instances[i].measurement_progress.progress_percentage),
               (uint16_t)((uint32_t)(cs_initiator_instances[i].measurement_progress.progress_percentage * 100.f)) % 100);

      cs_initiator_display_update_data(i,
                                       cs_initiator_instances[i].conn_handle,
                                       CS_INITIATOR_DISPLAY_STATUS_CONNECTED,
                                       cs_initiator_instances[i].measurement_mainmode.distance_filtered,
                                       cs_initiator_instances[i].measurement_mainmode.distance_estimate_rssi,
                                       cs_initiator_instances[i].measurement_mainmode.likeliness,
                                       cs_initiator_instances[i].measurement_mainmode.bit_error_rate,
                                       cs_initiator_instances[i].measurement_mainmode.distance_raw,
                                       cs_initiator_instances[i].measurement_progress.progress_percentage,
                                       rtl_config.algo_mode,
                                       initiator_config.cs_main_mode);
    }
  }
  // Lazily subscribe to the tag's IMU notification once CS is up (armed by the
  // first CS result, so cs_initiator's own RAS GATT setup is complete) and the
  // GATT link is quiescent. Retried on busy until the CCCD write is accepted.
  if (cs_gate_enabled
      && (imu_conn != SL_BT_INVALID_CONNECTION_HANDLE)
      && imu_sub_arm
      && (imu_sub_state == IMU_SUB_IDLE)) {
    sl_status_t isc = sl_bt_gatt_set_characteristic_notification(imu_conn,
                                                                 IMU_DATA_CHAR_HANDLE,
                                                                 sl_bt_gatt_notification);
    if (isc == SL_STATUS_OK) {
      imu_sub_state = IMU_SUB_PENDING;
      log_info(APP_PREFIX "IMU notify: subscribing (handle %u)" NL,
               IMU_DATA_CHAR_HANDLE);
    }
    // else: stack busy with a CS GATT procedure; retry on the next tick.
  }
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

// -----------------------------------------------------------------------------
// Static function definitions

static void app_timer_callback(app_timer_t *timer, void *data)
{
  (void)timer;
  (void)data;
  cs_initiator_display_update();
}

/******************************************************************************
 * Return runtime configurable value for object tracking mode
 *****************************************************************************/
#if (SL_SIMPLE_BUTTON_COUNT > 1)
  #if CS_INITIATOR_DEFAULT_ALGO_MODE == SL_RTL_CS_ALGO_MODE_REAL_TIME_FAST
    #define CS_INITIATOR_ALTERNATIVE_ALGO_MODE SL_RTL_CS_ALGO_MODE_STATIC_HIGH_ACCURACY
  #else
    #define CS_INITIATOR_ALTERNATIVE_ALGO_MODE SL_RTL_CS_ALGO_MODE_REAL_TIME_FAST
  #endif
static uint8_t get_algo_mode(void)
{
  if (sl_button_get_state(SL_SIMPLE_BUTTON_INSTANCE(1)) == SL_SIMPLE_BUTTON_PRESSED) {
    return CS_INITIATOR_ALTERNATIVE_ALGO_MODE;
  }
  return CS_INITIATOR_DEFAULT_ALGO_MODE;
}
#else
static uint8_t get_algo_mode(void)
{
  return CS_INITIATOR_DEFAULT_ALGO_MODE;
}
#endif

/******************************************************************************
 * Get requested antenna usage configuration as string
 *****************************************************************************/
static const char *antenna_usage_to_str(const cs_initiator_config_t *config)
{
  if (config->cs_main_mode == sl_bt_cs_mode_rtt) {
    switch (config->cs_sync_antenna_req) {
      case CS_SYNC_ANTENNA_1:
        return "antenna ID 1";
      case CS_SYNC_ANTENNA_2:
        return "antenna ID 2";
      case CS_SYNC_SWITCHING:
        return "switch between all antenna IDs";
      default:
        return "unknown";
    }
  } else {
    switch (config->cs_tone_antenna_config_idx_req) {
      case CS_ANTENNA_CONFIG_INDEX_SINGLE_ONLY:
        return "single antenna on both sides (1:1)";
      case CS_ANTENNA_CONFIG_INDEX_DUAL_I_SINGLE_R:
        return "dual antenna initiator & single antenna reflector (2:1)";
      case CS_ANTENNA_CONFIG_INDEX_SINGLE_I_DUAL_R:
        return "single antenna initiator & dual antenna reflector (1:2)";
      case CS_ANTENNA_CONFIG_INDEX_DUAL_ONLY:
        return "dual antennas on both sides (2:2)";
      default:
        return "unknown";
    }
  }
}

/******************************************************************************
 * Get algo mode as string
 *****************************************************************************/
static const char *algo_mode_to_str(uint8_t algo_mode)
{
  switch (algo_mode) {
    case SL_RTL_CS_ALGO_MODE_REAL_TIME_BASIC:
      return "real time basic (moving)";
    case SL_RTL_CS_ALGO_MODE_STATIC_HIGH_ACCURACY:
      return "stationary object tracking";
    case SL_RTL_CS_ALGO_MODE_REAL_TIME_FAST:
      return "real time fast (moving)";
    default:
      return "unknown";
  }
}

/******************************************************************************
 * Get instance number based on connection handle
 *****************************************************************************/
static sl_status_t get_instance_number(uint8_t conn_handle, uint8_t *instance_num)
{
  for (uint8_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    if (cs_initiator_instances[i].conn_handle == conn_handle) {
      *instance_num = i;
      return SL_STATUS_OK;
    }
  }
  return SL_STATUS_FAIL;
}

/******************************************************************************
 * Save connection
 *****************************************************************************/
static sl_status_t save_connection(uint8_t conn_handle)
{
  for (uint8_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    if (cs_initiator_instances[i].conn_handle == SL_BT_INVALID_CONNECTION_HANDLE) {
      cs_initiator_instances[i].conn_handle = conn_handle;
      return SL_STATUS_OK;
    }
  }
  return SL_STATUS_FULL;
}

/******************************************************************************
 * Extract measurement results
 *****************************************************************************/
static void cs_on_result(const uint8_t conn_handle,
                         const uint16_t ranging_counter,
                         const uint8_t *result,
                         const cs_result_session_data_t *result_data,
                         const cs_ranging_data_t *ranging_data,
                         const void *user_data)
{
  (void)ranging_data;
  (void)user_data;
  uint8_t initiator_num;

  if (result != NULL) {
    sl_status_t sc = get_instance_number(conn_handle, &initiator_num);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to get instance number for connection! [sc: 0x%lx]" NL,
                conn_handle,
                sc);
      return;
    }

    sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                 CS_RESULT_FIELD_DISTANCE_MAINMODE,
                                 (uint8_t *)result,
                                 (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.distance_filtered);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to extract distance! [sc: 0x%lx]" NL,
                conn_handle,
                sc);
    }

    if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
      sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                   CS_RESULT_FIELD_DISTANCE_SUBMODE,
                                   (uint8_t *)result,
                                   (uint8_t *)&cs_initiator_instances[initiator_num].measurement_submode.distance_filtered);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to extract sub mode distance! [sc: 0x%lx]" NL,
                  conn_handle,
                  sc);
      }
    }

    sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                 CS_RESULT_FIELD_DISTANCE_RAW_MAINMODE,
                                 (uint8_t *)result,
                                 (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.distance_raw);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to extract RAW distance! [sc: 0x%lx]" NL,
                conn_handle,
                sc);
    }

    if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
      sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                   CS_RESULT_FIELD_DISTANCE_RAW_SUBMODE,
                                   (uint8_t *)result,
                                   (uint8_t *)&cs_initiator_instances[initiator_num].measurement_submode.distance_raw);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to extract sub mode RAW distance! [sc: 0x%lx]" NL,
                  conn_handle,
                  sc);
      }
    }

    sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                 CS_RESULT_FIELD_LIKELINESS_MAINMODE,
                                 (uint8_t *)result,
                                 (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.likeliness);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to extract likeliness! [sc: 0x%lx]" NL,
                conn_handle,
                sc);
    }

    if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
      sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                   CS_RESULT_FIELD_LIKELINESS_SUBMODE,
                                   (uint8_t *)result,
                                   (uint8_t *)&cs_initiator_instances[initiator_num].measurement_submode.likeliness);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to extract sub mode likeliness! [sc: 0x%lx]" NL,
                  conn_handle,
                  sc);
      }
    }

    if (rtl_config.algo_mode == SL_RTL_CS_ALGO_MODE_REAL_TIME_FAST
        && initiator_config.cs_main_mode == sl_bt_cs_mode_pbr
        && (initiator_config.channel_map_preset == CS_CHANNEL_MAP_PRESET_HIGH
            || initiator_config.channel_map_preset == CS_CHANNEL_MAP_PRESET_MEDIUM)) {
      sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                   CS_RESULT_FIELD_VELOCITY_MAINMODE,
                                   (uint8_t *)result,
                                   (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.velocity);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to extract velocity! [sc: 0x%lx]" NL,
                  conn_handle,
                  sc);
      }
    }

    // BER is only for RTT
    if (initiator_config.cs_main_mode == sl_bt_cs_mode_rtt) {
      sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                   CS_RESULT_FIELD_BIT_ERROR_RATE,
                                   (uint8_t *)result,
                                   (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.bit_error_rate);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to extract BER! [sc: 0x%lx]" NL,
                  conn_handle,
                  sc);
      }
    }

    // Extract RSSI distance always
    sc = cs_result_extract_field((cs_result_session_data_t *)result_data,
                                 CS_RESULT_FIELD_DISTANCE_RSSI,
                                 (uint8_t *)result,
                                 (uint8_t *)&cs_initiator_instances[initiator_num].measurement_mainmode.distance_estimate_rssi);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to extract RSSI distance! [sc: 0x%lx]" NL,
                conn_handle,
                sc);
    }
    cs_initiator_instances[initiator_num].measurement_arrived = true;
    cs_initiator_instances[initiator_num].measurement_cnt++;
    cs_initiator_instances[initiator_num].ranging_counter = ranging_counter;
    // CS is producing results: the connection's GATT setup is complete, so it
    // is now safe to subscribe to the tag's IMU notification.
    imu_sub_arm = true;
  } else {
    log_info(APP_INSTANCE_PREFIX "RTL process skipped!" NL,
             conn_handle);
  }
}

/******************************************************************************
 * Extract intermediate results between measurement results
 * Note: only called when stationary object tracking used
 *****************************************************************************/
static void cs_on_intermediate_result(const cs_intermediate_result_t * intermediate_result, const void *user_data)
{
  (void) user_data;
  uint8_t instance_num;
  if (intermediate_result != NULL) {
    sl_status_t sc = get_instance_number(intermediate_result->connection, &instance_num);
    if (sc != SL_STATUS_OK) {
      log_error(APP_INSTANCE_PREFIX "Failed to get instance number for connection" NL,
                intermediate_result->connection);
      return;
    }
    memcpy(&cs_initiator_instances[instance_num].measurement_progress,
           intermediate_result,
           sizeof(cs_intermediate_result_t));
    cs_initiator_instances[instance_num].measurement_progress_changed = true;
  }
}

/******************************************************************************
 * Check if the CLI has changed any default values of the initiator
 *****************************************************************************/
static void check_cli_values(void)
{
#ifdef SL_CATALOG_CS_INITIATOR_CLI_PRESENT
  if (cs_initiator_cli_get_antenna_config_index() != initiator_config.cs_tone_antenna_config_idx_req) {
    antenna_set_pbr = true;
  }
  initiator_config.cs_tone_antenna_config_idx_req = cs_initiator_cli_get_antenna_config_index();
  if (cs_initiator_cli_get_cs_sync_antenna_usage() != initiator_config.cs_sync_antenna_req) {
    antenna_set_rtt = true;
  }
  initiator_config.cs_sub_mode = cs_initiator_cli_get_sub_mode();
  initiator_config.cs_sync_antenna_req = cs_initiator_cli_get_cs_sync_antenna_usage();
  initiator_config.cs_main_mode = cs_initiator_cli_get_mode();
  initiator_config.conn_phy = cs_initiator_cli_get_conn_phy();
  initiator_config.max_procedure_count = cs_initiator_cli_get_procedure_counter();
  rtl_config.algo_mode = cs_initiator_cli_get_algo_mode();
  initiator_config.channel_map_preset = cs_initiator_cli_get_preset();
  cs_initiator_apply_channel_map_preset(initiator_config.channel_map_preset,
                                        initiator_config.channel_map.data);
#endif // SL_CATALOG_CS_INITIATOR_CLI_PRESENT
}

/******************************************************************************
 * Create new initiator instance
 *****************************************************************************/
static sl_status_t create_new_initiator_instance(uint8_t conn_handle)
{
  sl_status_t sc;
  cs_intermediate_result_t measurement_progress;
  log_info(APP_INSTANCE_PREFIX "Creating new initiator instance" NL, conn_handle);
  // Slot accounting is handled by save_connection() / delete_initiator_instance();
  // the array itself is sized CS_INITIATOR_MAX_CONNECTIONS, so save_connection()
  // already rejects connections that exceed the cap. Re-checking here against
  // the array occupancy would always reject the first connection (its slot has
  // just been allocated in BLE_PEER_MANAGER_ON_CONN_OPENED_CENTRAL).
  uint8_t i;
  sc = get_instance_number(conn_handle, &i);
  if (sc != SL_STATUS_OK) {
    log_error(APP_PREFIX "Failed to get instance number for new connection! [sc: 0x%lx]" NL,
              sc);
    return sc;
  }
  // Store the new initiator instance
  cs_initiator_instances[i].measurement_cnt = 0u;
  memset(&cs_initiator_instances[i].measurement_mainmode, 0u, sizeof(cs_measurement_data_t));
  memset(&cs_initiator_instances[i].measurement_submode, 0u, sizeof(cs_measurement_data_t));
  memset(&cs_initiator_instances[i].measurement_progress, 0u, sizeof(measurement_progress));

  sc = cs_initiator_create(conn_handle,
                           &initiator_config,
                           &rtl_config,
                           cs_on_result,
                           cs_on_intermediate_result,
                           cs_on_error,
                           NULL);
  if (sc != SL_STATUS_OK) {
    log_error(APP_INSTANCE_PREFIX "Failed to create initiator instance, "
                                  "error:0x%lx" NL,
              conn_handle,
              sc);
    (void)ble_peer_manager_central_close_connection(conn_handle);
  }
  return sc;
}

/******************************************************************************
 * Check if the remote device supports the required capabilities
 *****************************************************************************/
static void check_supported_capabilities(const sl_bt_msg_t *evt)
{
  sl_status_t sc;
  uint8_t local_cs_sync_phy;
  uint16_t local_subfeatures;
  uint8_t local_roles;
  uint8_t local_rtt_aa_only;
  uint8_t local_rtt_sounding;
  uint8_t local_rtt_random;
  sc = sl_bt_cs_read_local_supported_capabilities(NULL,
                                                  NULL,
                                                  &initiator_config.num_antennas,
                                                  NULL,
                                                  &local_roles,
                                                  NULL,
                                                  NULL,
                                                  &local_rtt_aa_only,
                                                  &local_rtt_sounding,
                                                  &local_rtt_random,
                                                  NULL,
                                                  NULL,
                                                  &local_cs_sync_phy,
                                                  &local_subfeatures,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  NULL);
  app_assert_status(sc);
  // initiator config is set to CS_SYNC_PHY 2M
  // but local/remote device only supports CS_SYNC_PHY 1M
  if (initiator_config.cs_sync_phy == sl_bt_gap_phy_2m
      && (local_cs_sync_phy == 0 || evt->data.evt_cs_read_remote_supported_capabilities_complete.cs_sync_phys == 0)) {
    app_log_error(APP_PREFIX "Requested CS SYNC PHY (%d) is not supported by the local/remote device (%d)" APP_LOG_NL,
                  initiator_config.cs_sync_phy,
                  local_cs_sync_phy);
    initiator_config.cs_sync_phy = sl_bt_gap_phy_1m;
    app_log_info(APP_PREFIX "Using CS SYNC PHY %d instead" APP_LOG_NL,
                 initiator_config.cs_sync_phy);
  }
  // initiator config algorithm #3c is selected but local/remote device doesn't support it
  if (initiator_config.channel_selection_type == sl_bt_cs_channel_selection_algorithm_3c
      && ((local_subfeatures >> CS_CHANNEL_SELECTION_ALGORITHM_3C_SUBFEATURE_BITMASK & 0x01) == 0
          || (evt->data.evt_cs_read_remote_supported_capabilities_complete.subfeatures
              >> CS_CHANNEL_SELECTION_ALGORITHM_3C_SUBFEATURE_BITMASK & 0x01) == 0)) {
    app_assert(false, APP_PREFIX "Requested CS channel selection algorithm #3c is not supported by the local/remote device" APP_LOG_NL);
  }
  // local device doesn't support initiator role
  // or remote device doesn't support reflector role
  if ((local_roles >> CS_ROLE_INITIATOR_SUBFEATURE_BITMASK & 0x01) == 0
      || (evt->data.evt_cs_read_remote_supported_capabilities_complete.roles
          >> CS_ROLE_REFLECTOR_SUBFEATURE_BITMASK & 0x01) == 0) {
    app_assert(false, APP_PREFIX "Requested CS role is not supported by the local/remote device" APP_LOG_NL);
  }
  // if mode is RTT check the RTT capabilities
  if (initiator_config.cs_main_mode == sl_bt_cs_mode_rtt) {
    // [RTT AA only] is set but local/remote device doesn't support it
    if ((initiator_config.rtt_type == sl_bt_cs_rtt_type_aa_only)
        && (local_rtt_aa_only == 0
            || evt->data.evt_cs_read_remote_supported_capabilities_complete.rtt_aa_only == 0)) {
      app_assert(false, APP_PREFIX "RTT AA only is not supported by the local/remote device" APP_LOG_NL);
    }
    // [RTT Sounding] is set but local/remote device doesn't support it
    if ((initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_32_bit_sounding
         || initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_96_bit_sounding)
        && (local_rtt_sounding == 0
            || evt->data.evt_cs_read_remote_supported_capabilities_complete.rtt_sounding == 0)) {
      app_assert(false, APP_PREFIX "RTT sounding is not supported by the local/remote device" APP_LOG_NL);
    }
    // [RTT Random] is set but local/remote device doesn't support it
    if ((initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_32_bit_random
         || initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_64_bit_random
         || initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_96_bit_random
         || initiator_config.rtt_type == sl_bt_cs_rtt_type_fractional_128_bit_random)
        && (local_rtt_random == 0
            || evt->data.evt_cs_read_remote_supported_capabilities_complete.rtt_random_payload == 0)) {
      app_assert(false, APP_PREFIX "RTT random is not supported by the local/remote device" APP_LOG_NL);
    }
  }
}

/******************************************************************************
 * Write measurement results to the display and to the iostream
 *****************************************************************************/

static void print_head_and_data(cs_initiator_instances_t *initiator)
{
      const bd_addr *bt_address = ble_peer_manager_get_bt_address(initiator->conn_handle);
      for (uint8_t is_data = ((measurement_counter % CS_INITIATOR_HEADER_LOG) > 0); is_data <= 1; is_data++) {
        log_info(APP_INSTANCE_PREFIX, initiator->conn_handle);
        cs_initiator_print_bt_address(!is_data, bt_address);

        cs_initiator_print_result(CS_RESULT_FIELD_DISTANCE_MAINMODE,
                                  !is_data,
                                  &(initiator->measurement_mainmode.distance_filtered));
        // Distance submode
        if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
          cs_initiator_print_result(CS_RESULT_FIELD_DISTANCE_SUBMODE,
                                    !is_data,
                                    &(initiator->measurement_submode.distance_filtered));
        }
        // Distance RAW
        cs_initiator_print_result(CS_RESULT_FIELD_DISTANCE_RAW_MAINMODE,
                                  !is_data,
                                  &(initiator->measurement_mainmode.distance_raw));
        // Distance submode RAW
        if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
          cs_initiator_print_result(CS_RESULT_FIELD_DISTANCE_RAW_SUBMODE,
                                    !is_data,
                                    &(initiator->measurement_submode.distance_raw));
        }
        // Likeliness
        cs_initiator_print_result(CS_RESULT_FIELD_LIKELINESS_MAINMODE,
                                  !is_data,
                                  &(initiator->measurement_mainmode.likeliness));
        // Likeliness submode
        if (initiator_config.cs_sub_mode != sl_bt_cs_submode_disabled) {
          cs_initiator_print_result(CS_RESULT_FIELD_LIKELINESS_SUBMODE,
                                    !is_data,
                                    &(initiator->measurement_submode.likeliness));
        }
        // RSSI distance
        cs_initiator_print_result(CS_RESULT_FIELD_DISTANCE_RSSI,
                                  !is_data,
                                  &(initiator->measurement_mainmode.distance_estimate_rssi));
        // Velocity
        cs_initiator_print_result(CS_RESULT_FIELD_VELOCITY_MAINMODE,
                                  !is_data,
                                  &(initiator->measurement_mainmode.velocity));

        // BER
        cs_initiator_print_result(CS_RESULT_FIELD_BIT_ERROR_RATE,
                                  !is_data,
                                  &(initiator->measurement_mainmode.bit_error_rate));
        log_append(APP_LOG_NL);
      }
}

/******************************************************************************
 * Delete initiator instance
 *****************************************************************************/
static void delete_initiator_instance(uint8_t conn_handle)
{
  for (uint32_t i = 0u; i < CS_INITIATOR_MAX_CONNECTIONS; i++) {
    if (cs_initiator_instances[i].conn_handle == conn_handle) {
      cs_initiator_instances[i].conn_handle = SL_BT_INVALID_CONNECTION_HANDLE;
      cs_initiator_instances[i].measurement_cnt = 0u;
      memset(&cs_initiator_instances[i].measurement_mainmode, 0u, sizeof(cs_measurement_data_t));
      memset(&cs_initiator_instances[i].measurement_submode, 0u, sizeof(cs_measurement_data_t));
      memset(&cs_initiator_instances[i].measurement_progress, 0u, sizeof(cs_intermediate_result_t));
      cs_initiator_instances[i].measurement_arrived = false;
      cs_initiator_instances[i].measurement_progress_changed = false;
      cs_initiator_instances[i].read_remote_capabilities = false;
      cs_initiator_instances[i].security_increased = false;
      break;
    }
  }
}

/******************************************************************************
 * CS error handler
 *****************************************************************************/
static void cs_on_error(uint8_t conn_handle, cs_error_event_t err_evt, sl_status_t sc)
{
  switch (err_evt) {
    // Assert
    case CS_ERROR_EVENT_CS_PROCEDURE_STOP_TIMER_FAILED:
    case CS_ERROR_EVENT_CS_PROCEDURE_UNEXPECTED_DATA:
      app_assert(false,
                 APP_INSTANCE_PREFIX "Unrecoverable CS procedure error happened!"
                                     "[E: 0x%x sc: 0x%lx]" NL,
                 conn_handle,
                 err_evt,
                 (unsigned long)sc);
      break;

    // Discard
    case CS_ERROR_EVENT_RTL_PROCESS_ERROR:
      log_error(APP_INSTANCE_PREFIX "RTL processing error happened!"
                                    "[E: 0x%x rtl_err: 0x%lx]" NL,
                conn_handle,
                err_evt,
                (unsigned long)sc);
      break;

    case CS_ERROR_EVENT_INITIATOR_FAILED_TO_SET_INTERVALS:
      log_error(APP_INSTANCE_PREFIX "Failed to set CS procedure scheduling!"
                                    "[E: 0x%x sc: 0x%lx]" NL,
                conn_handle,
                err_evt,
                (unsigned long)sc);
      break;
    // Antenna usage not supported
    case CS_ERROR_EVENT_INITIATOR_PBR_ANTENNA_USAGE_NOT_SUPPORTED:
      if (antenna_set_pbr) {
        log_error(APP_INSTANCE_PREFIX "The requested PBR antenna configuration is not supported!"
                                      " Will use the closest one and continue."
                                      "[E: 0x%x sc: 0x%lx]" NL,
                  conn_handle,
                  err_evt,
                  (unsigned long)sc);
      } else {
        log_info(APP_INSTANCE_PREFIX "Default PBR antenna configuration not supported!"
                                     " Will use the closest one and continue."
                                     "[E: 0x%x sc: 0x%lx]" NL,
                 conn_handle,
                 err_evt,
                 (unsigned long)sc);
      }
      break;
    case CS_ERROR_EVENT_INITIATOR_RTT_ANTENNA_USAGE_NOT_SUPPORTED:
      if (antenna_set_rtt) {
        log_error(APP_INSTANCE_PREFIX "The requested RTT antenna configuration is not supported!"
                                      " Will use the closest one and continue."
                                      "[E: 0x%x sc: 0x%lx]" NL,
                  conn_handle,
                  err_evt,
                  (unsigned long)sc);
      } else {
        log_info(APP_INSTANCE_PREFIX "Default RTT antenna configuration not supported!"
                                     " Will use the closest one and continue."
                                     "[E: 0x%x sc: 0x%lx]" NL,
                 conn_handle,
                 err_evt,
                 (unsigned long)sc);
      }
      break;

    case CS_ERROR_EVENT_RAS_CLIENT_REALTIME_RECEIVE_FAILED:
      log_error(APP_INSTANCE_PREFIX "RAS reception error!"
                                    "[E: 0x%x sc: 0x%lx]" NL,
                conn_handle,
                err_evt,
                (unsigned long)sc);
      break;

    // Close connection
    default:
      log_error(APP_INSTANCE_PREFIX "Error happened! Closing connection."
                                    "[E: 0x%x sc: 0x%lx]" NL,
                conn_handle,
                err_evt,
                (unsigned long)sc);
      // Common errors
      if (err_evt == CS_ERROR_EVENT_TIMER_ELAPSED) {
        log_error(APP_INSTANCE_PREFIX "Operation timeout." NL, conn_handle);
      } else if (err_evt == CS_ERROR_EVENT_INITIATOR_FAILED_TO_INCREASE_SECURITY) {
        log_error(APP_INSTANCE_PREFIX "Security level increase failed." NL, conn_handle);
      }
      // Close the connection
      app_log_info(APP_INSTANCE_PREFIX "Closing connection" NL, conn_handle);
      sl_status_t status = ble_peer_manager_central_close_connection(conn_handle);
      // If closing the connection fails no connnection_closed event will be received
      // so we need to restart scanning here if needed
      if (status != SL_STATUS_OK) {
        sc = gated_start_scanning();
        app_assert_status(sc);
      }
      break;
  }
}

// -----------------------------------------------------------------------------
// Event / callback definitions

/**************************************************************************//**
 * Bluetooth stack event handler
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t * evt)
{
  sl_status_t sc;
  uint8_t instance_num;
  const char* device_name = REFLECTOR_DEVICE_NAME;
  sc = on_event_security(evt);
  if (sc != SL_STATUS_OK) {
    log_error(APP_PREFIX "Security event handler failed: 0x%04lx" NL, (unsigned long)sc);
  }

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
    {
      // Set TX power
      int16_t min_tx_power_x10 = SYSTEM_MIN_TX_POWER_DBM * 10;
      int16_t max_tx_power_x10 = SYSTEM_MAX_TX_POWER_DBM * 10;
      sc = sl_bt_system_set_tx_power(min_tx_power_x10,
                                     max_tx_power_x10,
                                     &min_tx_power_x10,
                                     &max_tx_power_x10);
      app_assert_status(sc);
      log_info(APP_PREFIX "Minimum system TX power is set to: %d dBm" NL, min_tx_power_x10 / 10);
      log_info(APP_PREFIX "Maximum system TX power is set to: %d dBm" NL, max_tx_power_x10 / 10);

      // Reset to initial state
      ble_peer_manager_central_init();
      ble_peer_manager_filter_init();
      cs_initiator_init();

      // Print the Bluetooth address
      bd_addr address;
      uint8_t address_type;
      sc = sl_bt_gap_get_identity_address(&address, &address_type);
      app_assert_status(sc);
      log_info(APP_PREFIX "Bluetooth %s address: %02X:%02X:%02X:%02X:%02X:%02X\n",
               address_type ? "static random" : "public device",
               address.addr[5],
               address.addr[4],
               address.addr[3],
               address.addr[2],
               address.addr[1],
               address.addr[0]);

      sc = cs_antenna_configure(CS_INITIATOR_ANTENNA_OFFSET);
      app_assert_status(sc);

      // Filter for advertised name (CS_RFLCT)
      sc = ble_peer_manager_set_filter_device_name(device_name,
                                                   strlen(device_name),
                                                   false);
      app_assert_status(sc);

      uint16_t ras_service_uuid = CS_RAS_SERVICE_UUID;
      sc = ble_peer_manager_set_filter_service_uuid16((sl_bt_uuid_16_t *)&ras_service_uuid);
      app_assert_status(sc);

#ifndef SL_CATALOG_CS_INITIATOR_CLI_PRESENT
      // VCOM gate: boot idle (default STOP) so a mid-session initiator reset
      // comes up in AoA mode and never auto-reconnects. The host mode_controller
      // sends "GO\n" over VCOM to begin CS ranging.
      log_info(APP_PREFIX "CS gate idle (STOP) - waiting for GO on VCOM..." NL);
#else
      log_info("CS CLI is active." NL);
#endif // SL_CATALOG_CS_INITIATOR_CLI_PRESENT

      // Set PHY to 2M if possible
      sc = sl_bt_connection_set_default_preferred_phy(initiator_config.conn_phy,
                                                      sl_bt_gap_phy_any);
      app_assert_status(sc);
      break;
    }
    case sl_bt_evt_connection_parameters_id:
      sc = get_instance_number(evt->data.evt_connection_parameters.connection, &instance_num);
      // Initiator instance not created yet
      if (sc != SL_STATUS_OK) {
        break;
      }
      if (evt->data.evt_connection_parameters.security_mode != sl_bt_connection_mode1_level1) {
        if (!cs_initiator_instances[instance_num].read_remote_capabilities) {
          sc = sl_bt_cs_read_remote_supported_capabilities(evt->data.evt_connection_parameters.connection);
          app_assert_status(sc);
          cs_initiator_instances[instance_num].read_remote_capabilities = true;
#if CS_INIT_FORWARD_BOND
          // Link is now encrypted: forward the pinned peer identity once.
          bond_forward(evt->data.evt_connection_parameters.connection,
                       (uint8_t)evt->data.evt_connection_parameters.security_mode);
#endif
          log_info(APP_INSTANCE_PREFIX "Reading capabilities..." NL,
                   evt->data.evt_connection_parameters.connection);
        }
      } else {
        if (!cs_initiator_instances[instance_num].security_increased) {
          log_info(APP_INSTANCE_PREFIX "Increasing security..." NL,
                   evt->data.evt_connection_parameters.connection);
          sc = sl_bt_sm_increase_security(evt->data.evt_connection_parameters.connection);
          app_assert_status(sc);
          cs_initiator_instances[instance_num].security_increased = true;
        }
      }
      break;

    // --------------------------------
    // MTU exchange event
    case sl_bt_evt_gatt_mtu_exchanged_id:
    {
      initiator_config.mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
      log_info(APP_PREFIX "MTU set to: %u" NL,
               initiator_config.mtu);
    }
    break;

    case sl_bt_evt_cs_read_remote_supported_capabilities_complete_id:
    {
      uint16_t proc_interval;
      uint16_t conn_interval;
      // Save the configured antenna config index because the SDK repurposes
      // this field to carry the remote antenna count into
      // create_new_initiator_instance() (cs_initiator.c:658). We restore it
      // after the call so the next connection sees the configured value again.
      uint8_t cs_tone_antenna_config_index_temp = initiator_config.cs_tone_antenna_config_idx;
      uint8_t connection = evt->data.evt_cs_read_remote_supported_capabilities_complete.connection;
      check_supported_capabilities(evt);
      if (initiator_config.max_procedure_count == 0) {
        sc = cs_initiator_get_intervals(initiator_config.cs_main_mode,
                                        initiator_config.cs_sub_mode,
                                        initiator_config.procedure_scheduling,
                                        initiator_config.channel_map_preset,
                                        rtl_config.algo_mode,
                                        initiator_config.cs_tone_antenna_config_idx,
                                        initiator_config.use_real_time_ras_mode,
                                        &conn_interval,
                                        &proc_interval);
        if (sc == SL_STATUS_NOT_SUPPORTED) {
          log_info(APP_INSTANCE_PREFIX "Parameter optimization is not supported with the given input parameters" NL, connection);
        } else if (sc == SL_STATUS_IDLE) {
          log_info(APP_PREFIX "No optimization - using custom procedure scheduling" NL);
        } else if (sc == SL_STATUS_OK) {
          initiator_config.max_connection_interval = initiator_config.min_connection_interval = conn_interval;
          initiator_config.max_procedure_interval = initiator_config.min_procedure_interval = proc_interval;
          log_info(APP_INSTANCE_PREFIX "Optimized parameters for connection interval and procedure interval." NL, connection);
        } else {
          log_error(APP_INSTANCE_PREFIX "Invalid input, cannot optimize parameters." NL, connection);
        }
        float period_ms = initiator_config.max_connection_interval * 1.25f * initiator_config.max_procedure_interval;
        log_info(APP_INSTANCE_PREFIX "Connection interval: %u  Procedure interval: %u  Period: %d ms  Frequency: %u.%03u Hz" NL,
                 connection,
                 initiator_config.max_connection_interval,
                 initiator_config.max_procedure_interval,
                 (int)period_ms,
                 (uint16_t)(1000.0f / period_ms),
                 (((uint16_t)(1000000.0f / period_ms)) % 1000));
      }
      // The SDK reads initiator_config.cs_tone_antenna_config_idx as the
      // REMOTE antenna count inside create_new_initiator_instance. Set it
      // before the call and restore it after.
      initiator_config.cs_tone_antenna_config_idx = evt->data.evt_cs_read_remote_supported_capabilities_complete.num_antennas;
      sc = create_new_initiator_instance(connection);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to create initiator instance, "
                                      "error:0x%lx" NL,
                  connection,
                  sc);
        (void)ble_peer_manager_central_close_connection(connection);
      } else {
        log_info(APP_INSTANCE_PREFIX "New initiator instance created" NL,
                 connection);
      }
      // Restore the antenna config index for subsequent connections.
      initiator_config.cs_tone_antenna_config_idx = cs_tone_antenna_config_index_temp;
      sc = get_instance_number(connection, &instance_num);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Failed to get instance number for connection" NL,
                  connection);
        return;
      }
      // Scan for new reflector connections if we have room for more
      if (count_active_reflector_connections() < CS_INITIATOR_MAX_CONNECTIONS) {
        sc = gated_start_scanning();
        app_assert_status(sc);
      }
      break;
    }
    // -------------------------------
    // This event indicates that the BT stack buffer resources were exhausted
    case sl_bt_evt_system_resource_exhausted_id:
      log_error(APP_PREFIX "BT stack buffers exhausted, data loss may have occurred! "
                           "buf_discarded='%u' buf_alloc_fail='%u' heap_alloc_fail='%u'" APP_LOG_NL,
                evt->data.evt_system_resource_exhausted.num_buffers_discarded,
                evt->data.evt_system_resource_exhausted.num_buffer_allocation_failures,
                evt->data.evt_system_resource_exhausted.num_heap_allocation_failures);
      break;

    // -------------------------------
    // IMU piggyback: a notification arrived from the tag's IMU Data
    // characteristic. Forward the raw 24-byte frame to the host over VCOM.
    // (RAS notifications use different handles and are processed internally by
    // the cs_initiator component, so filtering on the IMU handle is safe.)
    case sl_bt_evt_gatt_characteristic_value_id:
      if ((evt->data.evt_gatt_characteristic_value.connection == imu_conn)
          && (evt->data.evt_gatt_characteristic_value.characteristic == IMU_DATA_CHAR_HANDLE)
          && (evt->data.evt_gatt_characteristic_value.att_opcode
              == sl_bt_gatt_handle_value_notification)) {
        imu_forward_frame(evt->data.evt_gatt_characteristic_value.value.data,
                          evt->data.evt_gatt_characteristic_value.value.len);
      }
      break;

    // -------------------------------
    // GATT procedure finished. We only track our own IMU CCCD write here; once
    // a notification subscription was queued (IMU_SUB_PENDING) the next
    // completion on that connection is ours (the stack serializes procedures).
    case sl_bt_evt_gatt_procedure_completed_id:
      if ((evt->data.evt_gatt_procedure_completed.connection == imu_conn)
          && (imu_sub_state == IMU_SUB_PENDING)) {
        if (evt->data.evt_gatt_procedure_completed.result == SL_STATUS_OK) {
          imu_sub_state = IMU_SUB_DONE;
          log_info(APP_PREFIX "IMU notify: enabled" NL);
        } else {
          // Subscription failed (e.g. raced a CS procedure); retry next tick.
          imu_sub_state = IMU_SUB_IDLE;
        }
      }
      break;

#if defined(SL_CATALOG_BLUETOOTH_FEATURE_EXTERNAL_BONDING_DATABASE_PRESENT)
    // -------------------------------
    // External Bonding Database (CS auth Layer 2, ephemeral keystore).
    // We persist nothing: answer every data request with 0-length ("not
    // found") so each connection performs a fresh SC pairing -- identical to
    // the first-connect path the initiator already handles. On each pairing the
    // stack hands us the peer's keys via the data event; we lift the tag's IRK
    // (type 0x6) and, when enabled, forward it to the host.
    case sl_bt_evt_external_bondingdb_data_request_id:
      (void)sl_bt_external_bondingdb_set_data(
          evt->data.evt_external_bondingdb_data_request.connection,
          evt->data.evt_external_bondingdb_data_request.type,
          0u, NULL);
      break;
    case sl_bt_evt_external_bondingdb_data_id:
#if CS_INIT_FORWARD_IRK
      if (evt->data.evt_external_bondingdb_data.type
          == sl_bt_external_bondingdb_data_irk) {
        irk_forward(evt->data.evt_external_bondingdb_data.data.data,
                    evt->data.evt_external_bondingdb_data.data.len);
      }
#endif // CS_INIT_FORWARD_IRK
      break;
    case sl_bt_evt_external_bondingdb_local_irk_request_id:
      {
        uint8_t local_irk[16];
        ebd_get_local_irk(local_irk);
        (void)sl_bt_external_bondingdb_set_local_irk(sizeof(local_irk),
                                                    local_irk);
      }
      break;
    case sl_bt_evt_external_bondingdb_data_ready_id:
      // Stack has all bonding data; the encrypted link is now usable.
      break;
#endif // SL_CATALOG_BLUETOOTH_FEATURE_EXTERNAL_BONDING_DATABASE_PRESENT

    default:
      break;
  }
}

#if (SL_SIMPLE_BUTTON_COUNT > 1)
void sl_button_on_change(const sl_button_t *handle)
{
  if (security_is_confirmation_in_progress()) {
    if (handle == SL_SIMPLE_BUTTON_INSTANCE(0)) {
      security_set_confirmation(SECURITY_ALLOW_CONFIRMATION);
    } else if (handle == SL_SIMPLE_BUTTON_INSTANCE(1)) {
      security_set_confirmation(SECURITY_DENY_CONFIRMATION);
    }
  }
}
#endif // SL_SIMPLE_BUTTON_COUNT > 1

/******************************************************************************
 * BLE peer manager event handler
 *
 * @param[in] evt Event coming from the peer manager.
 *****************************************************************************/
void ble_peer_manager_on_event_initiator(ble_peer_manager_evt_type_t * event)
{
  sl_status_t sc;
  bd_addr *address;

  switch (event->evt_id) {
    case BLE_PEER_MANAGER_ON_CONN_OPENED_CENTRAL:
      sc = save_connection(event->connection_id);
      if (sc != SL_STATUS_OK) {
        log_error(APP_INSTANCE_PREFIX "Error finding a slot for connection: "
                                      "dropping connection..." NL,
                  event->connection_id);
        (void)ble_peer_manager_central_close_connection(event->connection_id);
        break;
      }
      address = ble_peer_manager_get_bt_address(event->connection_id);
      log_info(APP_INSTANCE_PREFIX "Connection opened as central with CS Reflector"
                                   " '%02X:%02X:%02X:%02X:%02X:%02X'" NL,
               event->connection_id,
               address->addr[5],
               address->addr[4],
               address->addr[3],
               address->addr[2],
               address->addr[1],
               address->addr[0]);
      check_cli_values();
      cs_initiator_display_set_measurement_mode(initiator_config.cs_main_mode, rtl_config.algo_mode);
      // Arm IMU piggyback for this connection; the actual subscription is
      // deferred until CS is producing results (see cs_on_result / app_process_action).
      imu_conn = event->connection_id;
      imu_sub_state = IMU_SUB_IDLE;
      imu_sub_arm = false;

      break;
    case BLE_PEER_MANAGER_ON_CONN_CLOSED:
      log_info(APP_INSTANCE_PREFIX "Connection closed" NL, event->connection_id);
      if (event->connection_id == imu_conn) {
        imu_conn = SL_BT_INVALID_CONNECTION_HANDLE;
        imu_sub_state = IMU_SUB_IDLE;
        imu_sub_arm = false;
      }
      sc = cs_initiator_delete(event->connection_id);
      if ((sc == SL_STATUS_NOT_FOUND) || (sc == SL_STATUS_INVALID_HANDLE)) {
        log_info(APP_INSTANCE_PREFIX "Initiator instance not found" NL, event->connection_id);
      } else {
        app_assert_status(sc);
        log_info(APP_INSTANCE_PREFIX "Initiator instance removed" NL, event->connection_id);
      }
      delete_initiator_instance(event->connection_id);
      // If a STOP is waiting on the link to actually drop, ack now that it has.
      if (gate_stop_ack_pending && (count_active_reflector_connections() == 0u)) {
        gate_stop_ack_pending = false;
        gate_send("[APP] STOPPED\n");
      }
      // Only rescan if the host gate still wants CS; a STOP-triggered close
      // must stay closed so the tag returns to AoA mode.
      sc = gated_start_scanning();
      app_assert_status(sc);
      break;

    case BLE_PEER_MANAGER_ERROR:
      log_error(APP_INSTANCE_PREFIX "Peer Manager error" NL,
                event->connection_id);
      break;

    default:
      log_info(APP_INSTANCE_PREFIX "Unhandled Peer Manager event (%u)" NL,
               event->connection_id,
               event->evt_id);
      break;
  }
}
