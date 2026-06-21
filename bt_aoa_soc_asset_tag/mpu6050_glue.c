/***************************************************************************//**
 * @file mpu6050_glue.c
 * @brief Platform glue connecting the libdriver MPU6050 driver to the
 *        Silicon Labs EFR32 I2C / sleeptimer APIs used by this project.
 *
 * The libdriver/mpu6050 platform-independent driver (third_party/mpu6050)
 * needs us to provide:
 *   - I2C init / deinit / multi-byte read / multi-byte write
 *   - blocking ms delay
 *   - debug printf
 *   - irq receive callback (left as a no-op; we poll the FIFO)
 *
 * I2C bus, pins and GPIO routing are kept here so the driver works whether
 * or not anything else in app.c has touched the bus yet. The init is
 * idempotent so calling it multiple times (from app.c and from the driver)
 * is safe.
 ******************************************************************************/
#include "driver_mpu6050_interface.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "em_i2c.h"
#include "sl_sleeptimer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

// Must match the I2C / pin choices in app.c (BRD4108A I2C1 on PD02/PD03).
#define GLUE_I2C                  I2C1
#define GLUE_I2C_NO               1U
#define GLUE_I2C_CLOCK            cmuClock_I2C1
#define GLUE_I2C_SCL_PORT         gpioPortD
#define GLUE_I2C_SCL_PIN          2U
#define GLUE_I2C_SDA_PORT         gpioPortD
#define GLUE_I2C_SDA_PIN          3U
#define GLUE_I2C_TIMEOUT_CYCLES   40000U

static bool s_bus_initialized = false;

static int glue_i2c_transfer(I2C_TransferSeq_TypeDef *seq)
{
  I2C_TransferReturn_TypeDef state;
  uint32_t timeout = GLUE_I2C_TIMEOUT_CYCLES;

  state = I2C_TransferInit(GLUE_I2C, seq);
  while (state == i2cTransferInProgress && timeout > 0U) {
    state = I2C_Transfer(GLUE_I2C);
    timeout--;
  }
  return (state == i2cTransferDone) ? 0 : 1;
}

uint8_t mpu6050_interface_iic_init(void)
{
  I2C_Init_TypeDef init = I2C_INIT_DEFAULT;

  if (s_bus_initialized) {
    return 0;
  }

  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(GLUE_I2C_CLOCK, true);

  GPIO_PinModeSet(GLUE_I2C_SCL_PORT, GLUE_I2C_SCL_PIN, gpioModeWiredAndPullUp, 1U);
  GPIO_PinModeSet(GLUE_I2C_SDA_PORT, GLUE_I2C_SDA_PIN, gpioModeWiredAndPullUp, 1U);

  GLUE_I2C->EN_CLR = I2C_EN_EN;
  GPIO->I2CROUTE[GLUE_I2C_NO].ROUTEEN = 0U;
  GPIO->I2CROUTE[GLUE_I2C_NO].SCLROUTE =
      ((uint32_t)GLUE_I2C_SCL_PORT << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
      | ((uint32_t)GLUE_I2C_SCL_PIN << _GPIO_I2C_SCLROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[GLUE_I2C_NO].SDAROUTE =
      ((uint32_t)GLUE_I2C_SDA_PORT << _GPIO_I2C_SDAROUTE_PORT_SHIFT)
      | ((uint32_t)GLUE_I2C_SDA_PIN << _GPIO_I2C_SDAROUTE_PIN_SHIFT);
  GPIO->I2CROUTE[GLUE_I2C_NO].ROUTEEN =
      GPIO_I2C_ROUTEEN_SCLPEN | GPIO_I2C_ROUTEEN_SDAPEN;

  I2C_Init(GLUE_I2C, &init);
  s_bus_initialized = true;
  return 0;
}

uint8_t mpu6050_interface_iic_deinit(void)
{
  // Leave the bus up; other sensors (BMP280) also share it.
  return 0;
}

// libdriver passes the 8-bit I2C address (e.g. 0xD0). emlib expects the
// same left-shifted form in seq.addr, so we just forward it as-is.
uint8_t mpu6050_interface_iic_read(uint8_t addr, uint8_t reg,
                                   uint8_t *buf, uint16_t len)
{
  I2C_TransferSeq_TypeDef seq = {
    .addr = (uint16_t)addr,
    .flags = I2C_FLAG_WRITE_READ,
    .buf = {
      { .data = &reg, .len = 1U },
      { .data = buf,  .len = len }
    }
  };
  if (buf == NULL || len == 0U) {
    return 1;
  }
  return (uint8_t)glue_i2c_transfer(&seq);
}

uint8_t mpu6050_interface_iic_write(uint8_t addr, uint8_t reg,
                                    uint8_t *buf, uint16_t len)
{
  // Two-buffer write: register address followed by payload, without
  // copying into a temporary buffer. emlib stitches them on the wire.
  I2C_TransferSeq_TypeDef seq = {
    .addr = (uint16_t)addr,
    .flags = I2C_FLAG_WRITE_WRITE,
    .buf = {
      { .data = &reg, .len = 1U },
      { .data = buf,  .len = len }
    }
  };
  if (len == 0U) {
    // Register-only write.
    I2C_TransferSeq_TypeDef short_seq = {
      .addr = (uint16_t)addr,
      .flags = I2C_FLAG_WRITE,
      .buf = {
        { .data = &reg, .len = 1U },
        { .data = NULL, .len = 0U }
      }
    };
    return (uint8_t)glue_i2c_transfer(&short_seq);
  }
  if (buf == NULL) {
    return 1;
  }
  return (uint8_t)glue_i2c_transfer(&seq);
}

void mpu6050_interface_delay_ms(uint32_t ms)
{
  sl_sleeptimer_delay_millisecond(ms);
}

void mpu6050_interface_debug_print(const char *const fmt, ...)
{
  // No serial console wired up here; swallow driver log output. Pull in
  // app_log here later if debugging the DMP load is needed.
  (void)fmt;
}

void mpu6050_interface_receive_callback(uint8_t type)
{
  (void)type; // Polled mode: we drain the FIFO ourselves each tick.
}

void mpu6050_interface_dmp_tap_callback(uint8_t count, uint8_t direction)
{
  (void)count;
  (void)direction;
}

void mpu6050_interface_dmp_orient_callback(uint8_t orientation)
{
  (void)orientation;
}
