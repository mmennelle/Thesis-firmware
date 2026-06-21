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
// CTE advertiser gating during CS
// ---------------------------------------------------------------------------
// The Silabs proprietary CTE rides on a dedicated non-connectable extended
// advertising set that the `gatt_service_cte_silabs` component creates and
// starts at 20 ms (SL_GATT_SERVICE_CTE_SILABS_ADV_INTERVAL). That 20 ms tone
// stream is the dominant radio-time consumer on the tag and starves both the
// CS reflector procedures and the IMU broadcaster (PIPELINE §2.1 / §7.4).
//
// The only peer that ever opens a CONNECTION to this tag is the CS initiator;
// the AoA locator receives CTE connectionlessly (plain extended advertising,
// no periodic-sync) and never connects. An open connection is therefore a
// reliable "CS is active" signal. While a connection is up we stretch the CTE
// advertising interval to its maximum so the tone stream effectively stops,
// handing that airtime back to CS and the IMU; on disconnect we restore the
// 20 ms cadence. The locator simply sees a gap in CTE reports and re-acquires
// within one advertising interval once CTE resumes.
//
// `adv_cte_interval` and `adv_cte_start()` are non-static symbols provided by
// the gatt_service_cte_adv component (sl_gatt_service_cte_silabs.c). They are
// declared here so the app can drive them without pulling the component's
// internal header; the types match the component's adv_cte_interval_t.
extern uint16_t adv_cte_interval;        // CTE adv interval, units of 0.625 ms
extern sl_status_t adv_cte_start(void);  // (re)start CTE adv with current params

#define CTE_ADV_INTERVAL_ACTIVE  ((uint16_t)SL_GATT_SERVICE_CTE_SILABS_ADV_INTERVAL) // 20 ms
#define CTE_ADV_INTERVAL_PAUSED  ((uint16_t)0xFFFFU)  // ~40.96 s => effectively off

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

// Rebuild and push the IMU advertising payload. Layout is byte-for-byte
// identical to the MPU6050 tag:
//   Flags(3) + ManufacturerData(28) = 31 bytes (legacy adv budget).
//   payload = magic(2), ver(1), seq(1), accel(6), gyro(6), quaternion(8).
static void update_imu_advertising_packet(void)
{
  uint8_t adv_data[31];
  uint8_t offset = 0;
  imu_sample_t sample;
  const bool sample_valid = imu_read_sample(&sample);
  sl_status_t sc;

  if (!bluetooth_ready || imu_set_handle == 0xFFU) {
    return;
  }

  if (sample_valid) {
    sample.status |= IMU_STATUS_SAMPLE_VALID;
  }
  (void)sample.status; // status not transmitted (legacy adv budget)

  // Flags AD structure (3 bytes).
  adv_data[offset++] = 2U;
  adv_data[offset++] = AD_TYPE_FLAGS;
  adv_data[offset++] = AD_FLAG_LE_GENERAL_DISCOVERABLE | AD_FLAG_BR_EDR_NOT_SUPPORTED;

  // Manufacturer specific AD structure.
  // Length = type(1) + company_id(2) + payload(24) = 27.
  adv_data[offset++] = 27U;
  adv_data[offset++] = AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
  write_u16_le(&adv_data[offset], SILABS_COMPANY_ID);
  offset += 2U;

  write_u16_le(&adv_data[offset], IMU_PAYLOAD_MAGIC);
  offset += 2U;
  adv_data[offset++] = IMU_PAYLOAD_VERSION;
  adv_data[offset++] = imu_sequence++;

  write_u16_le(&adv_data[offset], (uint16_t)sample.accel_x_mg);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.accel_y_mg);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.accel_z_mg);
  offset += 2U;

  write_u16_le(&adv_data[offset], (uint16_t)sample.gyro_x_dps);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.gyro_y_dps);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.gyro_z_dps);
  offset += 2U;

  write_u16_le(&adv_data[offset], (uint16_t)sample.quat_x_x10000);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.quat_y_x10000);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.quat_z_x10000);
  offset += 2U;
  write_u16_le(&adv_data[offset], (uint16_t)sample.quat_w_x10000);
  offset += 2U;

  // IMU set is non-connectable and dedicated — never paused by connections,
  // so we only refresh its data here.
  sc = sl_bt_legacy_advertiser_set_data(imu_set_handle, 0, offset, adv_data);
  (void)sc;
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

  imu_update_period_ticks = imu_timer_freq_hz / IMU_ADV_UPDATE_RATE_HZ;
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
        // 160 * 0.625 ms = 100 ms => 10 Hz on-air. Lowered from 50 Hz to
        // free radio time for the Silabs proprietary CTE extended adv
        // (gatt_service_cte_silabs @ 20 ms) — that one is what the AoA
        // locator syncs to, and it was being starved when the IMU set
        // ran at the same 20 ms interval. Bridge consumes imu/raw at
        // ~10–20 Hz anyway, so no downstream loss.
        (void)sl_bt_advertiser_set_timing(imu_set_handle, 160, 160, 0, 0);
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
        sl_status_t rc = cs_reflector_create(conn, &cs_reflector_config);
        if (rc != SL_STATUS_OK) {
          app_log_warning("[APP] cs_reflector_create(%u) -> 0x%04lx" APP_LOG_NL,
                          conn, (unsigned long)rc);
        }
      }
      // A connection on this tag means a CS initiator (the AoA locator never
      // connects). Pause the 20 ms CTE tone stream so the CS procedures and
      // the IMU broadcaster reclaim the airtime; restored on disconnect.
      if (connection_count == 1) {
        cte_pause();
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
        // CS is done — bring the CTE tone stream back to its 20 ms cadence
        // so the AoA locator can re-acquire.
        cte_resume();
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
