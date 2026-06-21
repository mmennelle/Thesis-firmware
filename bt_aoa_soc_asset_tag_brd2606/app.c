/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
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
#include "app_assert.h"
#include "app_log.h"
#include "sl_bluetooth.h"
#include "sl_main_init.h"
#include "sl_sleeptimer.h"
#include "gatt_db.h"

// Onboard inertial sensor: the BRD2606A carries an InvenSense ICM-40627
// 6-axis IMU on EUSART1 SPI with a board enable line (SL_BOARD_SENSOR_IMU,
// GPIO PC09). The Silicon Labs IMU fusion service (`sl_imu`) drives the
// ICM-40627 driver and produces filtered acceleration, gyro, and an
// orientation (Euler) estimate. We convert that orientation to a unit
// quaternion so the over-the-air manufacturer payload matches the format
// previously broadcast by the (retired) external MPU6050 bolt-on, keeping
// the host-side parser unchanged.
#include "sl_board_control.h"
#include "sl_imu.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

// CS reflector POC: the same connectable advertising set carries the CTE
// service for the AoA locator AND the RAS service (auto-contributed by
// `cs_ras_server`) for a CS initiator. When any peer connects we spin up a
// reflector instance for that connection; the CS handshake will simply
// never start if the peer is the AoA locator, and the reflector instance
// goes away on disconnect.
#include "cs_antenna.h"
#include "cs_reflector.h"
#include "cs_reflector_config.h"
#include "cs_sync_antenna.h"

// Default CTE advertising interval (SL_GATT_SERVICE_CTE_SILABS_ADV_INTERVAL)
// used when restoring the tone stream after a CS session ends.
#include "sl_gatt_service_cte_silabs_config.h"

// The advertising set handle allocated from Bluetooth stack (connectable:
// AoA CTE + CS/RAS).
static uint8_t advertising_set_handle = 0xff;

// Dedicated non-connectable advertising set carrying the IMU manufacturer
// payload. It is never preempted by connection events so the inertial
// stream keeps broadcasting while a CS initiator / AoA locator is connected.
static uint8_t imu_set_handle = 0xff;

// Number of active connections.
static uint8_t connection_count = 0;

// Reflector defaults mirror the standalone CS reflector example.
static cs_reflector_config_t cs_reflector_config = {
  .max_tx_power_dbm = CS_REFLECTOR_MAX_TX_POWER_DBM,
  .cs_sync_antenna  = CS_REFLECTOR_CS_SYNC_ANTENNA
};

// ---------------------------------------------------------------------------
// AoA <-> CS mode switching, driven by connection state
// ---------------------------------------------------------------------------
// The CS initiator firmware is gated so it ONLY opens a connection to this tag
// when CS ranging is actually wanted (in AoA mode it stays disconnected). That
// makes "a connection is open" a true, race-free "CS mode active" signal here:
//
//   * Connected (CS mode):  pause the 20 ms Silabs CTE tone stream so the CS
//     reflector procedures get the radio, and push the IMU broadcaster up to
//     10 Hz now that CTE is no longer hogging airtime.
//   * Disconnected (AoA mode): restore the 20 ms CTE stream so the AoA locator
//     re-acquires, and drop the IMU broadcaster back to 3 Hz so it yields
//     airtime to CTE.
//
// CS "speed" itself is driven entirely by the initiator's procedure timing;
// the tag's only lever is freeing the radio (CTE off), which is exactly what
// lets the reflector keep up at the initiator's maximum cadence.
//
// `adv_cte_interval` and `adv_cte_start()` are non-static symbols provided by
// the gatt_service_cte_adv component (sl_gatt_service_cte_silabs.c). They are
// declared here so the app can drive them without pulling the component's
// internal header.
extern uint16_t adv_cte_interval;        // CTE adv interval, units of 0.625 ms
extern sl_status_t adv_cte_start(void);  // (re)start CTE adv with current params

#define CTE_ADV_INTERVAL_ACTIVE  ((uint16_t)SL_GATT_SERVICE_CTE_SILABS_ADV_INTERVAL) // 20 ms
#define CTE_ADV_INTERVAL_PAUSED  ((uint16_t)0xFFFFU)  // ~40.96 s => effectively off

// IMU broadcast rates for the two modes (Hz).
#define IMU_RATE_AOA_HZ                     3U   // AoA mode: yield airtime to CTE
#define IMU_RATE_CS_HZ                      10U  // CS mode: CTE paused, room to run

// Pause the CTE tone stream (CS session starting).
static void cte_pause(void)
{
  adv_cte_interval = CTE_ADV_INTERVAL_PAUSED;
  (void)adv_cte_start();
}

// Restore the 20 ms CTE tone stream (CS session ended).
static void cte_resume(void)
{
  adv_cte_interval = CTE_ADV_INTERVAL_ACTIVE;
  (void)adv_cte_start();
}

// ---------------------------------------------------------------------------
// IMU advertising
// ---------------------------------------------------------------------------

// Rate at which we refresh the IMU advertising payload. The fusion service
// is configured to sample at the same rate; sl_imu_is_data_ready() gates the
// actual read so we never block.
#define IMU_ADV_UPDATE_RATE_HZ              50U

// AD structure and payload definitions (identical to the MPU6050 tag so the
// host parser is unchanged).
#define AD_TYPE_FLAGS                       0x01U
#define AD_TYPE_MANUFACTURER_SPECIFIC_DATA  0xFFU
#define AD_FLAG_LE_GENERAL_DISCOVERABLE     0x02U
#define AD_FLAG_BR_EDR_NOT_SUPPORTED        0x04U

// Bluetooth SIG company identifier for Silicon Labs (little-endian in payload).
#define SILABS_COMPANY_ID                   0x02FFU

// Custom payload metadata for host-side parser.
#define IMU_PAYLOAD_MAGIC                   0x494DU // 'IM'
#define IMU_PAYLOAD_VERSION                 1U
// Length of the raw IMU payload shared by the advertisement (after company id)
// and the GATT notification: magic(2)+ver(1)+seq(1)+accel(6)+gyro(6)+quat(8).
#define IMU_PAYLOAD_LEN                     24U
#define IMU_STATUS_SAMPLE_VALID             0x01U
#define IMU_STATUS_IMU_INIT_OK              0x02U
#define IMU_STATUS_IMU_READ_OK              0x08U

typedef struct {
  int16_t accel_x_mg;
  int16_t accel_y_mg;
  int16_t accel_z_mg;
  int16_t gyro_x_dps;
  int16_t gyro_y_dps;
  int16_t gyro_z_dps;
  int16_t quat_x_x10000;
  int16_t quat_y_x10000;
  int16_t quat_z_x10000;
  int16_t quat_w_x10000;
  uint8_t status;
} imu_sample_t;

static bool bluetooth_ready = false;
static bool imu_ready = false;
static uint8_t imu_sequence = 0;
static uint32_t imu_last_update_tick = 0;
static uint32_t imu_update_period_ticks = 1;
static uint32_t imu_timer_freq_hz = 32768U;

// CS-piggyback transport state. While a peer is connected, a standalone
// advertising set is starved to ~2-3 Hz, so we stream IMU samples over the
// connection via GATT notifications instead. active_connection holds the
// current peer handle (0xFF = none) and imu_notify_enabled tracks whether the
// peer has subscribed to the IMU Data characteristic (CCCD = notify).
static uint8_t active_connection = 0xFFU;
static bool imu_notify_enabled = false;

// Last good IMU output. Quaternion defaults to identity so adv packets are
// valid before the first fused sample arrives.
static int16_t imu_last_accel_mg[3] = { 0, 0, 0 };
static int16_t imu_last_gyro_dps[3] = { 0, 0, 0 };
static int16_t imu_last_quat_x10000[4] = { 0, 0, 0, 10000 }; // x, y, z, w

static inline void write_u16_le(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)(value & 0xFFU);
  buf[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static int16_t sat_i16(int32_t value)
{
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return (int16_t)value;
}

// Convert a quaternion component in the range [-1, 1] to the int16 x10000
// over-the-air format used by the host parser.
static inline int16_t quat_to_x10000(float q)
{
  return sat_i16((int32_t)lrintf(q * 10000.0f));
}

// Convert the fusion service's Euler orientation (centidegrees, ZYX:
// roll about X, pitch about Y, yaw about Z) into a unit quaternion (x,y,z,w).
static void euler_centideg_to_quat(const int16_t ovec[3], int16_t quat_x10000[4])
{
  const float deg2rad = 0.017453292519943295f;
  float roll  = (float)ovec[0] * 0.01f * deg2rad;
  float pitch = (float)ovec[1] * 0.01f * deg2rad;
  float yaw   = (float)ovec[2] * 0.01f * deg2rad;

  float cr = cosf(roll * 0.5f);
  float sr = sinf(roll * 0.5f);
  float cp = cosf(pitch * 0.5f);
  float sp = sinf(pitch * 0.5f);
  float cy = cosf(yaw * 0.5f);
  float sy = sinf(yaw * 0.5f);

  float qw = cr * cp * cy + sr * sp * sy;
  float qx = sr * cp * cy - cr * sp * sy;
  float qy = cr * sp * cy + sr * cp * sy;
  float qz = cr * cp * sy - sr * sp * cy;

  quat_x10000[0] = quat_to_x10000(qx);
  quat_x10000[1] = quat_to_x10000(qy);
  quat_x10000[2] = quat_to_x10000(qz);
  quat_x10000[3] = quat_to_x10000(qw);
}

// One-shot init of the onboard IMU: power the sensor rail, then init and
// configure the fusion service. Best-effort: on failure the IMU adv path
// stays idle but CTE/CS continue to work.
static void imu_sensor_init(void)
{
  sl_status_t sc;

  (void)sl_board_enable_sensor(SL_BOARD_SENSOR_IMU);

  sc = sl_imu_init();
  if (sc != SL_STATUS_OK) {
    app_log_warning("[APP] sl_imu_init failed: 0x%04lx" APP_LOG_NL,
                    (unsigned long)sc);
    imu_ready = false;
    return;
  }

  sl_imu_configure((float)IMU_ADV_UPDATE_RATE_HZ);
  imu_ready = true;
  app_log_info("[APP] onboard ICM-40627 IMU ready" APP_LOG_NL);
}

// Pull the latest fused sample (if a new one is ready) into the cached
// accel / gyro / quaternion values. Returns true when fresh data was read.
static bool imu_read_sample(imu_sample_t *sample)
{
  uint8_t status = 0U;
  bool fresh = false;

  if (sample == NULL) {
    return false;
  }

  memset(sample, 0, sizeof(*sample));

  if (imu_ready) {
    status |= IMU_STATUS_IMU_INIT_OK;

    if (sl_imu_is_data_ready()) {
      int16_t avec[3];
      int16_t gvec[3];
      int16_t ovec[3];

      sl_imu_update();
      sl_imu_get_acceleration(avec); // milli-g
      sl_imu_get_gyro(gvec);         // centidegrees / s
      sl_imu_get_orientation(ovec);  // centidegrees (Euler)

      imu_last_accel_mg[0] = avec[0];
      imu_last_accel_mg[1] = avec[1];
      imu_last_accel_mg[2] = avec[2];

      // Service reports gyro in 0.01 dps; the OTA format is whole dps.
      imu_last_gyro_dps[0] = sat_i16(gvec[0] / 100);
      imu_last_gyro_dps[1] = sat_i16(gvec[1] / 100);
      imu_last_gyro_dps[2] = sat_i16(gvec[2] / 100);

      euler_centideg_to_quat(ovec, imu_last_quat_x10000);

      status |= IMU_STATUS_IMU_READ_OK;
      fresh = true;
    }
  }

  sample->accel_x_mg = imu_last_accel_mg[0];
  sample->accel_y_mg = imu_last_accel_mg[1];
  sample->accel_z_mg = imu_last_accel_mg[2];

  sample->gyro_x_dps = imu_last_gyro_dps[0];
  sample->gyro_y_dps = imu_last_gyro_dps[1];
  sample->gyro_z_dps = imu_last_gyro_dps[2];

  sample->quat_x_x10000 = imu_last_quat_x10000[0];
  sample->quat_y_x10000 = imu_last_quat_x10000[1];
  sample->quat_z_x10000 = imu_last_quat_x10000[2];
  sample->quat_w_x10000 = imu_last_quat_x10000[3];

  sample->status = status;
  return fresh;
}

// Rebuild and push the IMU sample. The same 24-byte payload feeds two
// transports:
//   * AoA mode (no connection): the dedicated non-connectable advertising set
//     (Flags(3) + ManufacturerData(28) = 31 bytes, legacy adv budget).
//   * CS/connected mode: a GATT notification over the active connection, since
//     a standalone advertising set is starved to ~2-3 Hz while connected.
//   payload = magic(2), ver(1), seq(1), accel(6), gyro(6), quaternion(8).
static void update_imu_advertising_packet(void)
{
  imu_sample_t sample;
  const bool sample_valid = imu_read_sample(&sample);

  if (!bluetooth_ready || imu_set_handle == 0xFFU) {
    return;
  }

  if (sample_valid) {
    sample.status |= IMU_STATUS_SAMPLE_VALID;
  }
  (void)sample.status; // status not transmitted (payload budget)

  // Build the shared 24-byte IMU payload once (single seq per fused sample).
  uint8_t payload[IMU_PAYLOAD_LEN];
  uint8_t p = 0;
  write_u16_le(&payload[p], IMU_PAYLOAD_MAGIC);
  p += 2U;
  payload[p++] = IMU_PAYLOAD_VERSION;
  payload[p++] = imu_sequence++;
  write_u16_le(&payload[p], (uint16_t)sample.accel_x_mg);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.accel_y_mg);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.accel_z_mg);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.gyro_x_dps);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.gyro_y_dps);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.gyro_z_dps);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.quat_x_x10000);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.quat_y_x10000);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.quat_z_x10000);
  p += 2U;
  write_u16_le(&payload[p], (uint16_t)sample.quat_w_x10000);
  p += 2U;
  // p == IMU_PAYLOAD_LEN here.

  // --- AoA transport: refresh the dedicated advertising set's payload. ---
  uint8_t adv_data[31];
  uint8_t offset = 0;
  adv_data[offset++] = 2U;
  adv_data[offset++] = AD_TYPE_FLAGS;
  adv_data[offset++] = AD_FLAG_LE_GENERAL_DISCOVERABLE | AD_FLAG_BR_EDR_NOT_SUPPORTED;
  // Manufacturer specific AD: len = type(1) + company_id(2) + payload(24) = 27.
  adv_data[offset++] = 27U;
  adv_data[offset++] = AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
  write_u16_le(&adv_data[offset], SILABS_COMPANY_ID);
  offset += 2U;
  memcpy(&adv_data[offset], payload, IMU_PAYLOAD_LEN);
  offset += IMU_PAYLOAD_LEN;
  (void)sl_bt_legacy_advertiser_set_data(imu_set_handle, 0, offset, adv_data);

  // --- CS/connected transport: notify the subscribed peer. ---
  // Scheduled connection events sustain the full 10 Hz that the starved
  // advertising set cannot deliver while connected.
  if ((active_connection != 0xFFU) && imu_notify_enabled) {
    (void)sl_bt_gatt_server_send_notification(active_connection,
                                              gattdb_imu_data,
                                              IMU_PAYLOAD_LEN,
                                              payload);
  }
}

// Set the IMU broadcast rate (on-air advertising cadence). The payload refresh
// period is matched to the broadcast period so every packet carries fresh
// fused data without needless SPI reads. Safe to call once the stack is up.
static void imu_set_rate(uint8_t rate_hz)
{
  if (rate_hz == 0U) {
    rate_hz = 1U;
  }
  imu_update_period_ticks = imu_timer_freq_hz / rate_hz;
  if (imu_update_period_ticks == 0U) {
    imu_update_period_ticks = 1U;
  }
  if (!bluetooth_ready || imu_set_handle == 0xFFU) {
    return;
  }
  // Advertising interval in 0.625 ms units: (1000 / rate_hz) ms / 0.625.
  uint32_t interval = 1600U / rate_hz;
  (void)sl_bt_advertiser_stop(imu_set_handle);
  (void)sl_bt_advertiser_set_timing(imu_set_handle, interval, interval, 0, 0);
  (void)sl_bt_legacy_advertiser_start(imu_set_handle,
                                      sl_bt_legacy_advertiser_non_connectable);
}

/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
void app_init(void)
{
  imu_timer_freq_hz = sl_sleeptimer_get_timer_frequency();
  if (imu_timer_freq_hz == 0U) {
    imu_timer_freq_hz = 32768U;
  }

  imu_update_period_ticks = imu_timer_freq_hz / IMU_RATE_AOA_HZ;
  if (imu_update_period_ticks == 0U) {
    imu_update_period_ticks = 1U;
  }

  // sl_imu_init() runs a blocking gyro calibration; keep the tag still at
  // power-up. Doing it here (before the BLE boot event) keeps the stack
  // event handler responsive.
  imu_sensor_init();
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
void app_process_action(void)
{
  uint32_t now_tick;

  if (!bluetooth_ready) {
    return;
  }

  now_tick = sl_sleeptimer_get_tick_count();
  if ((uint32_t)(now_tick - imu_last_update_tick) >= imu_update_period_ticks) {
    imu_last_update_tick = now_tick;
    update_imu_advertising_packet();
  }
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      // Allow the CS reflector to drive its max TX power range.
      {
        int16_t min_tx_power_x10 = CS_REFLECTOR_MIN_TX_POWER_DBM * 10;
        int16_t max_tx_power_x10 = CS_REFLECTOR_MAX_TX_POWER_DBM * 10;
        (void)sl_bt_system_set_tx_power(min_tx_power_x10,
                                        max_tx_power_x10,
                                        &min_tx_power_x10,
                                        &max_tx_power_x10);
      }

      // Apply the wireless/wired CS antenna offset table for brd2606.
      (void)cs_antenna_configure(CS_REFLECTOR_ANTENNA_OFFSET);

      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      app_assert_status(sc);

      // Generate data for advertising.
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Set advertising interval to 250 ms. The connectable legacy adv
      // is the CS reflector discovery beacon; once an initiator has bonded
      // it reconnects from cache, so a slower beacon costs only first-time
      // discovery latency and frees radio airtime for the CTE extended-adv
      // set (the AoA bottleneck — see PIPELINE §2.1 / §7.4). original == 100ms
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        400, // min. adv. interval (milliseconds * 1.6) = 250 ms original == 160
        400, // max. adv. interval (milliseconds * 1.6) = 250 ms original == 160
        0,   // adv. duration
        0);  // max. num. adv. events
      app_assert_status(sc);

      // Enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);

      // -------- IMU advertising set: non-connectable, dedicated broadcaster
      // for the onboard IMU manufacturer data. Never preempted by connection
      // events. Best-effort: if the stack cannot allocate a second set we do
      // NOT halt the MCU — CTE/CS already work above and the IMU update path
      // simply skips while imu_set_handle stays 0xFF.
      sc = sl_bt_advertiser_create_set(&imu_set_handle);
      if (sc != SL_STATUS_OK) {
        imu_set_handle = 0xFFU;
        app_log_warning("[APP] IMU adv set alloc failed: 0x%04lx" APP_LOG_NL,
                        (unsigned long)sc);
      } else {
        // Use the device identity address (same as the connectable set) so
        // the locator-side tag id matches across angle/ and imu/ topics.
        (void)sl_bt_advertiser_clear_random_address(imu_set_handle);
        // Boot into AoA mode: 3 Hz IMU broadcast (interval 533 * 0.625 ms =
        // 333 ms) so the IMU set yields airtime to the Silabs proprietary CTE
        // extended adv (gatt_service_cte_silabs @ 20 ms) — that one is what the
        // AoA locator syncs to. When a CS initiator connects we switch to CS
        // mode and bump this to 10 Hz (see imu_set_rate / connection events).
        (void)sl_bt_advertiser_set_timing(imu_set_handle, 533, 533, 0, 0);
        (void)sl_bt_legacy_advertiser_start(imu_set_handle,
                                            sl_bt_legacy_advertiser_non_connectable);
      }

      bluetooth_ready = true;
      imu_last_update_tick = sl_sleeptimer_get_tick_count();
      update_imu_advertising_packet();

      app_log_info("[APP] AoA tag + CS reflector + IMU POC ready" APP_LOG_NL);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      connection_count++;
      // Spin up a reflector instance for every inbound connection. If the
      // peer is the AoA locator (CTE only) the CS handshake will simply
      // never start and the reflector instance stays idle until disconnect.
      {
        uint8_t conn = evt->data.evt_connection_opened.connection;
        // Remember the peer so the IMU cadence loop can notify it. A fresh
        // connection has not yet subscribed to the IMU characteristic.
        active_connection = conn;
        imu_notify_enabled = false;
        sl_status_t rc = cs_reflector_create(conn, &cs_reflector_config);
        if (rc != SL_STATUS_OK) {
          app_log_warning("[APP] cs_reflector_create(%u) -> 0x%04lx" APP_LOG_NL,
                          conn, (unsigned long)rc);
        }
      }
      // Enter CS mode on the first connection: the gated initiator only
      // connects when it wants to range. Pause the CTE tone stream and push
      // the IMU broadcaster to 10 Hz now that CTE has freed the airtime.
      if (connection_count == 1) {
        cte_pause();
        imu_set_rate(IMU_RATE_CS_HZ);
      }
      // Do NOT re-advertise while a peer is connected. The CS initiator's
      // scanner is still running, so any new advertising packet would cause
      // it to open a duplicate connection (CS_INITIATOR_MAX_CONNECTIONS=1
      // → 0x1C / SL_STATUS_NO_MORE_RESOURCE → reconnect storm that starves
      // the primary CS procedure). Single-peer is fine for this POC.
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      {
        uint8_t conn = evt->data.evt_connection_closed.connection;
        if (conn == active_connection) {
          active_connection = 0xFFU;
          imu_notify_enabled = false;
        }
        if (cs_reflector_identify(conn)) {
          (void)cs_reflector_delete(conn);
        }
      }
      connection_count--;
      // Always resume advertising once we're free.
      if (connection_count == 0) {
        sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                           sl_bt_legacy_advertiser_connectable);
        app_assert_status(sc);
        // Back to AoA mode: restore the 20 ms CTE tone stream so the locator
        // re-acquires, and drop the IMU broadcaster to 3 Hz to yield airtime.
        cte_resume();
        imu_set_rate(IMU_RATE_AOA_HZ);
      }
      break;

    // -------------------------------
    // Peer (un)subscribed to a GATT characteristic. Track the IMU Data CCCD so
    // we only notify when the CS initiator has enabled notifications.
    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.characteristic
          == gattdb_imu_data) {
        uint8_t status_flags =
          evt->data.evt_gatt_server_characteristic_status.status_flags;
        if (status_flags == (uint8_t)sl_bt_gatt_server_client_config) {
          uint16_t cfg =
            evt->data.evt_gatt_server_characteristic_status.client_config_flags;
          imu_notify_enabled =
            ((cfg & (uint16_t)sl_bt_gatt_server_notification) != 0U);
        }
      }
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}
