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
#include "sl_bluetooth.h"
#include "sl_main_init.h"
#include "sl_sleeptimer.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "em_i2c.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>

// MPU6050 DMP support is provided by the vendored libdriver/mpu6050
// driver under third_party/. The "dmp" example layer uploads the
// InvenSense firmware blob, configures the FIFO at 50 Hz and exposes a
// single read entry point that drains accel / gyro / quaternion together.
#include "driver_mpu6050_dmp.h"

static bool dmp_ready = false;
static bool dmp_init_attempted = false;
// Last good DMP output. Quaternion defaults to identity so that adv
// packets remain valid before the first FIFO sample arrives.
static int16_t dmp_last_quat_x10000[4] = { 0, 0, 0, 10000 }; // x, y, z, w
static int16_t dmp_last_accel_mg[3] = { 0, 0, 0 };
static int16_t dmp_last_gyro_dps[3] = { 0, 0, 0 };
static int16_t dmp_last_temp_c_x100 = 0;

// libdriver returns the DMP quaternion as int32_t in Q30 fixed-point.
// Convert to int16_t scaled by 10000 (matches the over-the-air format).
static inline int16_t dmp_q30_to_x10000(int32_t q)
{
  int64_t scaled = ((int64_t)q * 10000) >> 30;
  if (scaled > 32767) {
    return 32767;
  }
  if (scaled < -32768) {
    return -32768;
  }
  return (int16_t)scaled;
}

// Two advertising sets are used so that AoA CTE connections and IMU
// manufacturer-data broadcasts can coexist:
//   - cte_set_handle : connectable extended adv carrying Flags +
//                       CTE Service UUID. The locator connects on this set
//                       for AoA. The stack pauses this set while a
//                       connection is open and we restart it on close.
//   - imu_set_handle : non-connectable extended adv carrying Flags + IMU
//                       manufacturer data. Always running, refreshed every
//                       IMU tick. Never preempted by connections.
static uint8_t cte_set_handle = 0xff;
static uint8_t imu_set_handle = 0xff;

// Number of active connections.
static uint8_t connection_count = 0;

// Update interval for refreshing IMU data in advertising payload.
#define IMU_ADV_UPDATE_RATE_HZ              70U

// AD structure and payload definitions.
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
#define IMU_STATUS_MPU_INIT_OK              0x02U
#define IMU_STATUS_BMP_INIT_OK              0x04U
#define IMU_STATUS_MPU_READ_OK              0x08U
#define IMU_STATUS_BMP_READ_OK              0x10U

// BRD4108A Rev A03 default I2C pins from board config (I2C1 on PD02/PD03).
#define SENSOR_I2C                          I2C1
#define SENSOR_I2C_NO                       1U
#define SENSOR_I2C_CLOCK                    cmuClock_I2C1
#define SENSOR_I2C_SCL_PORT                 gpioPortD
#define SENSOR_I2C_SCL_PIN                  2U
#define SENSOR_I2C_SDA_PORT                 gpioPortD
#define SENSOR_I2C_SDA_PIN                  3U

#define SENSOR_I2C_TIMEOUT_CYCLES           40000U

#define MPU9250_I2C_ADDR_0                  0x68U
#define MPU9250_I2C_ADDR_1                  0x69U
#define MPU9250_REG_WHO_AM_I                0x75U
#define MPU9250_REG_PWR_MGMT_1              0x6BU
#define MPU9250_REG_GYRO_CONFIG             0x1BU
#define MPU9250_REG_ACCEL_CONFIG            0x1CU
#define MPU9250_REG_ACCEL_XOUT_H            0x3BU

#define MPU_ACCEL_LSB_PER_G                 16384.0f
#define MPU_GYRO_LSB_PER_DPS                131.0f
#define DEG_TO_RAD                          0.01745329252f
#define ORIENTATION_KP                      2.0f

#define BMP280_I2C_ADDR_0                   0x76U
#define BMP280_I2C_ADDR_1                   0x77U
#define BMP280_REG_ID                       0xD0U
#define BMP280_REG_CALIB_START              0x88U
#define BMP280_REG_CTRL_MEAS                0xF4U
#define BMP280_REG_CONFIG                   0xF5U
#define BMP280_REG_PRESS_MSB                0xF7U

typedef struct {
  int16_t accel_x_mg;
  int16_t accel_y_mg;
  int16_t accel_z_mg;
  int16_t gyro_x_dps;
  int16_t gyro_y_dps;
  int16_t gyro_z_dps;
  int16_t pressure_hpa_x10;
  int16_t temperature_c_x100;
  int16_t quat_x_x10000;
  int16_t quat_y_x10000;
  int16_t quat_z_x10000;
  int16_t quat_w_x10000;
  uint8_t status;
} imu_sample_t;

static bool bluetooth_ready = false;
static uint8_t imu_sequence = 0;
static uint32_t imu_last_update_tick = 0;
static uint32_t imu_update_period_ticks = 1;
static uint32_t imu_timer_freq_hz = 32768U;
static bool sensor_bus_initialized = false;
static bool mpu_initialized = false;
static bool bmp_initialized = false;
static bool mpu_compatible_mode = false;
static uint8_t mpu_address = MPU9250_I2C_ADDR_0;
static uint8_t bmp_address = BMP280_I2C_ADDR_0;

typedef struct {
  uint16_t dig_t1;
  int16_t dig_t2;
  int16_t dig_t3;
  uint16_t dig_p1;
  int16_t dig_p2;
  int16_t dig_p3;
  int16_t dig_p4;
  int16_t dig_p5;
  int16_t dig_p6;
  int16_t dig_p7;
  int16_t dig_p8;
  int16_t dig_p9;
  int32_t t_fine;
} bmp280_calibration_t;

static bmp280_calibration_t bmp_cal;

static inline void write_u16_le(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)(value & 0xFFU);
  buf[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static sl_status_t sensor_i2c_transfer(I2C_TransferSeq_TypeDef *seq)
{
  I2C_TransferReturn_TypeDef state;
  uint32_t timeout = SENSOR_I2C_TIMEOUT_CYCLES;

  state = I2C_TransferInit(SENSOR_I2C, seq);
  while (state == i2cTransferInProgress && timeout > 0U) {
    state = I2C_Transfer(SENSOR_I2C);
    timeout--;
  }

  if (state == i2cTransferDone) {
    return SL_STATUS_OK;
  }

  return SL_STATUS_FAIL;
}

static sl_status_t sensor_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
  uint8_t tx[2] = { reg, value };
  I2C_TransferSeq_TypeDef seq = {
    .addr = (uint16_t)(addr << 1U),
    .flags = I2C_FLAG_WRITE,
    .buf = {
      { .data = tx, .len = sizeof(tx) },
      { .data = NULL, .len = 0U }
    }
  };

  return sensor_i2c_transfer(&seq);
}

static sl_status_t sensor_i2c_read_reg(uint8_t addr,
                                       uint8_t reg,
                                       uint8_t *data,
                                       uint16_t len)
{
  I2C_TransferSeq_TypeDef seq = {
    .addr = (uint16_t)(addr << 1U),
    .flags = I2C_FLAG_WRITE_READ,
    .buf = {
      { .data = &reg, .len = 1U },
      { .data = data, .len = len }
    }
  };

  if (data == NULL || len == 0U) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  return sensor_i2c_transfer(&seq);
}

static void sensor_i2c_init(void)
{
  I2C_Init_TypeDef init = I2C_INIT_DEFAULT;

  if (sensor_bus_initialized) {
    return;
  }

  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(SENSOR_I2C_CLOCK, true);

  GPIO_PinModeSet(SENSOR_I2C_SCL_PORT, SENSOR_I2C_SCL_PIN, gpioModeWiredAndPullUp, 1U);
  GPIO_PinModeSet(SENSOR_I2C_SDA_PORT, SENSOR_I2C_SDA_PIN, gpioModeWiredAndPullUp, 1U);

  SENSOR_I2C->EN_CLR = I2C_EN_EN;
  GPIO->I2CROUTE[SENSOR_I2C_NO].ROUTEEN = 0U;
  GPIO->I2CROUTE[SENSOR_I2C_NO].SCLROUTE =
    ((uint32_t)SENSOR_I2C_SCL_PORT << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
    | ((uint32_t)SENSOR_I2C_SCL_PIN << _GPIO_I2C_SCLROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[SENSOR_I2C_NO].SDAROUTE =
    ((uint32_t)SENSOR_I2C_SDA_PORT << _GPIO_I2C_SDAROUTE_PORT_SHIFT)
    | ((uint32_t)SENSOR_I2C_SDA_PIN << _GPIO_I2C_SDAROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[SENSOR_I2C_NO].ROUTEEN = GPIO_I2C_ROUTEEN_SCLPEN | GPIO_I2C_ROUTEEN_SDAPEN;

  I2C_Init(SENSOR_I2C, &init);
  sensor_bus_initialized = true;
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

// DMP quaternion read replaces fusion algorithm
static bool mpu9250_init(void)
{
  uint8_t who_am_i = 0;
  uint8_t raw[14];
  static const uint8_t mpu_candidates[] = {
    MPU9250_I2C_ADDR_0,
    MPU9250_I2C_ADDR_1
  };
  size_t i;

  if (mpu_initialized) {
    return true;
  }

  if (mpu_compatible_mode) {
    return true;
  }

  for (i = 0U; i < (sizeof(mpu_candidates) / sizeof(mpu_candidates[0])); i++) {
    mpu_address = mpu_candidates[i];

    if (sensor_i2c_read_reg(mpu_address, MPU9250_REG_WHO_AM_I, &who_am_i, 1U) != SL_STATUS_OK) {
      continue;
    }

    // Accept IDs used by MPU9250/MPU9255, MPU6500, and MPU6050-class compatible parts.
    if (who_am_i != 0x71U && who_am_i != 0x73U && who_am_i != 0x70U && who_am_i != 0x68U) {
      continue;
    }

    if (sensor_i2c_write_reg(mpu_address, MPU9250_REG_PWR_MGMT_1, 0x01U) != SL_STATUS_OK) {
      continue;
    }
    sl_sleeptimer_delay_millisecond(10U);

    if (sensor_i2c_write_reg(mpu_address, MPU9250_REG_GYRO_CONFIG, 0x00U) != SL_STATUS_OK) {
      continue;
    }
    if (sensor_i2c_write_reg(mpu_address, MPU9250_REG_ACCEL_CONFIG, 0x00U) != SL_STATUS_OK) {
      continue;
    }

    // libdriver's DMP init will reconfigure the chip the first time
    // imu_read_sample() runs; we just need to know an MPU lives at this
    // address so the address-pin enum (AD0_LOW / AD0_HIGH) is correct.
    mpu_initialized = true;
    return true;
  }

  // Fallback: if a device answers at 0x68/0x69 and exposes live data in the
  // MPU accel register window, keep using that raw layout even with an
  // unexpected WHO_AM_I value.
  for (i = 0U; i < (sizeof(mpu_candidates) / sizeof(mpu_candidates[0])); i++) {
    mpu_address = mpu_candidates[i];
    if (sensor_i2c_read_reg(mpu_address,
                            MPU9250_REG_ACCEL_XOUT_H,
                            raw,
                            sizeof(raw)) == SL_STATUS_OK) {
      mpu_compatible_mode = true;
      return true;
    }
  }

  return false;
}

static int32_t bmp280_compensate_temp_c_x100(int32_t adc_t)
{
  int32_t var1;
  int32_t var2;
  int32_t temperature;

  var1 = ((((adc_t >> 3) - ((int32_t)bmp_cal.dig_t1 << 1)))
          * ((int32_t)bmp_cal.dig_t2)) >> 11;
  var2 = (((((adc_t >> 4) - ((int32_t)bmp_cal.dig_t1))
            * ((adc_t >> 4) - ((int32_t)bmp_cal.dig_t1))) >> 12)
          * ((int32_t)bmp_cal.dig_t3)) >> 14;
  bmp_cal.t_fine = var1 + var2;
  temperature = (bmp_cal.t_fine * 5 + 128) >> 8;

  return temperature;
}

static uint32_t bmp280_compensate_press_pa(int32_t adc_p)
{
  int64_t var1;
  int64_t var2;
  int64_t p;

  var1 = ((int64_t)bmp_cal.t_fine) - 128000;
  var2 = var1 * var1 * (int64_t)bmp_cal.dig_p6;
  var2 = var2 + ((var1 * (int64_t)bmp_cal.dig_p5) << 17);
  var2 = var2 + (((int64_t)bmp_cal.dig_p4) << 35);
  var1 = ((var1 * var1 * (int64_t)bmp_cal.dig_p3) >> 8)
         + ((var1 * (int64_t)bmp_cal.dig_p2) << 12);
  var1 = (((((int64_t)1) << 47) + var1) * (int64_t)bmp_cal.dig_p1) >> 33;

  if (var1 == 0) {
    return 0U;
  }

  p = 1048576 - adc_p;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)bmp_cal.dig_p9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)bmp_cal.dig_p8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)bmp_cal.dig_p7) << 4);

  return (uint32_t)(p >> 8);
}

static bool bmp280_load_calibration(void)
{
  uint8_t calib[24];

  if (sensor_i2c_read_reg(bmp_address, BMP280_REG_CALIB_START, calib, sizeof(calib)) != SL_STATUS_OK) {
    return false;
  }

  bmp_cal.dig_t1 = (uint16_t)((uint16_t)calib[1] << 8U | calib[0]);
  bmp_cal.dig_t2 = (int16_t)((uint16_t)calib[3] << 8U | calib[2]);
  bmp_cal.dig_t3 = (int16_t)((uint16_t)calib[5] << 8U | calib[4]);
  bmp_cal.dig_p1 = (uint16_t)((uint16_t)calib[7] << 8U | calib[6]);
  bmp_cal.dig_p2 = (int16_t)((uint16_t)calib[9] << 8U | calib[8]);
  bmp_cal.dig_p3 = (int16_t)((uint16_t)calib[11] << 8U | calib[10]);
  bmp_cal.dig_p4 = (int16_t)((uint16_t)calib[13] << 8U | calib[12]);
  bmp_cal.dig_p5 = (int16_t)((uint16_t)calib[15] << 8U | calib[14]);
  bmp_cal.dig_p6 = (int16_t)((uint16_t)calib[17] << 8U | calib[16]);
  bmp_cal.dig_p7 = (int16_t)((uint16_t)calib[19] << 8U | calib[18]);
  bmp_cal.dig_p8 = (int16_t)((uint16_t)calib[21] << 8U | calib[20]);
  bmp_cal.dig_p9 = (int16_t)((uint16_t)calib[23] << 8U | calib[22]);

  return true;
}

static bool bmp280_try_init_at(uint8_t address)
{
  uint8_t id = 0U;

  bmp_address = address;
  if (sensor_i2c_read_reg(bmp_address, BMP280_REG_ID, &id, 1U) != SL_STATUS_OK) {
    return false;
  }
  if (id != 0x58U) {
    return false;
  }
  if (!bmp280_load_calibration()) {
    return false;
  }
  if (sensor_i2c_write_reg(bmp_address, BMP280_REG_CONFIG, 0x00U) != SL_STATUS_OK) {
    return false;
  }
  if (sensor_i2c_write_reg(bmp_address, BMP280_REG_CTRL_MEAS, 0x27U) != SL_STATUS_OK) {
    return false;
  }

  return true;
}

static bool bmp280_init(void)
{
  if (bmp_initialized) {
    return true;
  }

  if (bmp280_try_init_at(BMP280_I2C_ADDR_0) || bmp280_try_init_at(BMP280_I2C_ADDR_1)) {
    bmp_initialized = true;
    return true;
  }

  return false;
}

static bool ensure_dmp_init(void)
{
  // One-shot DMP firmware upload + FIFO setup. Runs the first time we
  // attempt a sample (so it happens after the BLE stack boots and we are
  // already pumping the main loop).
  if (dmp_ready || dmp_init_attempted) {
    return dmp_ready;
  }
  dmp_init_attempted = true;

  mpu6050_address_t addr_pin = (mpu_address == 0x69U)
                               ? MPU6050_ADDRESS_AD0_HIGH
                               : MPU6050_ADDRESS_AD0_LOW;

  if (mpu6050_dmp_init(addr_pin,
                       mpu6050_interface_receive_callback,
                       mpu6050_interface_dmp_tap_callback,
                       mpu6050_interface_dmp_orient_callback) == 0) {
    dmp_ready = true;
  }
  return dmp_ready;
}

static bool dmp_pull_latest(void)
{
  // The DMP FIFO produces a packet every 20 ms (50 Hz). Drain up to 4
  // packets per call so a 70 Hz adv tick never lets the FIFO overflow,
  // and use the most recent one as the live sample.
  int16_t accel_raw[4][3];
  float   accel_g[4][3];
  int16_t gyro_raw[4][3];
  float   gyro_dps[4][3];
  int32_t quat[4][4];
  float   pitch[4];
  float   roll[4];
  float   yaw[4];
  uint16_t len = 4U;

  if (!dmp_ready) {
    return false;
  }
  if (mpu6050_dmp_read_all(accel_raw, accel_g, gyro_raw, gyro_dps,
                           quat, pitch, roll, yaw, &len) != 0) {
    return false;
  }
  if (len == 0U) {
    return false;
  }

  uint16_t last = (uint16_t)(len - 1U);

  dmp_last_quat_x10000[0] = dmp_q30_to_x10000(quat[last][1]); // x
  dmp_last_quat_x10000[1] = dmp_q30_to_x10000(quat[last][2]); // y
  dmp_last_quat_x10000[2] = dmp_q30_to_x10000(quat[last][3]); // z
  dmp_last_quat_x10000[3] = dmp_q30_to_x10000(quat[last][0]); // w

  dmp_last_accel_mg[0] = sat_i16((int32_t)(accel_g[last][0] * 1000.0f));
  dmp_last_accel_mg[1] = sat_i16((int32_t)(accel_g[last][1] * 1000.0f));
  dmp_last_accel_mg[2] = sat_i16((int32_t)(accel_g[last][2] * 1000.0f));

  dmp_last_gyro_dps[0] = sat_i16((int32_t)gyro_dps[last][0]);
  dmp_last_gyro_dps[1] = sat_i16((int32_t)gyro_dps[last][1]);
  dmp_last_gyro_dps[2] = sat_i16((int32_t)gyro_dps[last][2]);

  return true;
}

static bool imu_read_sample(imu_sample_t *sample)
{
  uint8_t bmp_raw[6];
  bool mpu_ok = false;
  bool bmp_ok = false;
  uint8_t status = 0U;
  int32_t bmp_pressure_pa;
  int32_t adc_p;
  int32_t adc_t;
  float dt_sec;

  if (sample == NULL) {
    return false;
  }

  memset(sample, 0, sizeof(*sample));
  sample->quat_w_x10000 = 10000;
  dt_sec = (float)imu_update_period_ticks / (float)imu_timer_freq_hz;
  if (dt_sec <= 0.0f) {
    dt_sec = 1.0f / (float)IMU_ADV_UPDATE_RATE_HZ;
  }

  sensor_i2c_init();
  if (mpu9250_init()) {
    status |= IMU_STATUS_MPU_INIT_OK;
    if (ensure_dmp_init()) {
      // FIFO read may or may not have a fresh packet on any given tick;
      // either way, the "last good" values stay populated.
      (void)dmp_pull_latest();
      status |= IMU_STATUS_MPU_READ_OK;
      mpu_ok = true;

      sample->accel_x_mg = dmp_last_accel_mg[0];
      sample->accel_y_mg = dmp_last_accel_mg[1];
      sample->accel_z_mg = dmp_last_accel_mg[2];

      sample->gyro_x_dps = dmp_last_gyro_dps[0];
      sample->gyro_y_dps = dmp_last_gyro_dps[1];
      sample->gyro_z_dps = dmp_last_gyro_dps[2];

      sample->temperature_c_x100 = dmp_last_temp_c_x100;

      sample->quat_x_x10000 = dmp_last_quat_x10000[0];
      sample->quat_y_x10000 = dmp_last_quat_x10000[1];
      sample->quat_z_x10000 = dmp_last_quat_x10000[2];
      sample->quat_w_x10000 = dmp_last_quat_x10000[3];
    }
  }

  if (bmp280_init()) {
    status |= IMU_STATUS_BMP_INIT_OK;
    if (sensor_i2c_read_reg(bmp_address,
                            BMP280_REG_PRESS_MSB,
                            bmp_raw,
                            sizeof(bmp_raw)) == SL_STATUS_OK) {
      status |= IMU_STATUS_BMP_READ_OK;
      bmp_ok = true;

      adc_p = ((int32_t)bmp_raw[0] << 12)
              | ((int32_t)bmp_raw[1] << 4)
              | ((int32_t)bmp_raw[2] >> 4);
      adc_t = ((int32_t)bmp_raw[3] << 12)
              | ((int32_t)bmp_raw[4] << 4)
              | ((int32_t)bmp_raw[5] >> 4);

      (void)bmp280_compensate_temp_c_x100(adc_t);
      bmp_pressure_pa = (int32_t)bmp280_compensate_press_pa(adc_p);
      sample->pressure_hpa_x10 = sat_i16((bmp_pressure_pa + 5) / 10);
    }
  }

  sample->status = status;
  return (mpu_ok || bmp_ok);
}

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
  (void)sample.status; // status no longer transmitted (legacy adv budget)

  // Flags AD structure (3 bytes).
  adv_data[offset++] = 2U;
  adv_data[offset++] = AD_TYPE_FLAGS;
  adv_data[offset++] = AD_FLAG_LE_GENERAL_DISCOVERABLE | AD_FLAG_BR_EDR_NOT_SUPPORTED;

  // Manufacturer specific AD structure (legacy adv: 31-byte total cap).
  // Length = type(1) + company_id(2) + payload(24) = 27.
  // payload = magic(2), ver(1), seq(1), accel(6), gyro(6), quaternion(8).
  // (tick, pressure, temperature, status dropped to fit legacy budget.)
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

  // IMU set is non-connectable and dedicated — it is never paused by
  // connection events, so we only need to refresh its data here.
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
    case sl_bt_evt_system_boot_id: {
      // -------- CTE advertising set: connectable, carries CTE Service UUID
      // so the AoA locator discovers and connects on this set for AoA.
      sc = sl_bt_advertiser_create_set(&cte_set_handle);
      app_assert_status(sc);

      sc = sl_bt_advertiser_set_timing(cte_set_handle,
                                       160, // min adv interval (* 0.625ms)
                                       160, // max adv interval
                                       0,   // adv duration
                                       0);  // max adv events
      app_assert_status(sc);

      {
        // Flags(3) + Complete List of 16-bit Service UUIDs: 0x184A (4) = 7 bytes
        uint8_t cte_adv[7];
        cte_adv[0] = 2U;
        cte_adv[1] = AD_TYPE_FLAGS;
        cte_adv[2] = AD_FLAG_LE_GENERAL_DISCOVERABLE | AD_FLAG_BR_EDR_NOT_SUPPORTED;
        cte_adv[3] = 3U;
        cte_adv[4] = 0x03U; // Complete List of 16-bit Service UUIDs
        cte_adv[5] = 0x4AU; // CTE service UUID 0x184A LE
        cte_adv[6] = 0x18U;
        sc = sl_bt_extended_advertiser_set_data(cte_set_handle,
                                                sizeof(cte_adv),
                                                cte_adv);
        app_assert_status(sc);
      }

      sc = sl_bt_extended_advertiser_start(cte_set_handle,
                                           sl_bt_extended_advertiser_connectable,
                                           0);
      app_assert_status(sc);

      // -------- IMU advertising set: non-connectable, dedicated broadcaster
      // for IMU manufacturer data. Never preempted by connection events.
      // Best-effort: if the stack cannot allocate a second advertising set
      // (e.g. memory pool sized for one) we do NOT halt the MCU — CTE
      // already works above and the IMU update path simply skips when
      // imu_set_handle stays 0xFF.
      sc = sl_bt_advertiser_create_set(&imu_set_handle);
      if (sc != SL_STATUS_OK) {
        imu_set_handle = 0xFFU;
      } else {
        // Force this set to use the device public/identity address (same as
        // the CTE set) so the locator-side tag id matches across angle/ and
        // imu/ MQTT topics. Without this, the stack defaults to a random
        // non-resolvable address for a second adv set.
        sc = sl_bt_advertiser_clear_random_address(imu_set_handle);
        (void)sc;
        // 32 * 0.625 ms = 20 ms => ~50 Hz on-air, matched to the 70 Hz
        // DMP payload refresh rate while leaving slack for the 20 ms CTE
        // connection events.
        sc = sl_bt_advertiser_set_timing(imu_set_handle, 32, 32, 0, 0);
        (void)sc;
        sc = sl_bt_legacy_advertiser_start(imu_set_handle,
                                           sl_bt_legacy_advertiser_non_connectable);
        (void)sc;
      }

      // Mark ready and seed the first IMU advertising payload.
      bluetooth_ready = true;
      imu_last_update_tick = sl_sleeptimer_get_tick_count();
      update_imu_advertising_packet();
      break;
    }

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      connection_count++;
      // The stack pauses the CTE set when a connection opens on it. If the
      // stack allows further simultaneous connections, restart it so other
      // locators can still discover us. Best-effort.
      if (connection_count < SL_BT_CONFIG_MAX_CONNECTIONS) {
        (void)sl_bt_extended_advertiser_start(cte_set_handle,
                                              sl_bt_extended_advertiser_connectable,
                                              0);
      }
      // IMU set is independent and keeps running.
      (void)sc;
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      // If we were at max connections, the CTE set had been left paused.
      if (connection_count >= SL_BT_CONFIG_MAX_CONNECTIONS) {
        (void)sl_bt_extended_advertiser_start(cte_set_handle,
                                              sl_bt_extended_advertiser_connectable,
                                              0);
      }
      if (connection_count > 0U) {
        connection_count--;
      }
      (void)sc;
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
