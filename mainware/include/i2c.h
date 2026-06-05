#ifndef MAINWARE_I2C_H
#define MAINWARE_I2C_H

/* I2C3 HAL handle management (the handle lives in SRAM at 0x20009B04 — the same
 * handle the EEPROM uses). The de-init/init pair brackets the SCL bit-bang
 * bus-recovery in clock_pulse_gpioa8_until_pc9. */

/* De-init (abort/reset) the I2C3 peripheral handle. OEM 0x0803C8E4. */
void i2c3_handle_deinit(void);

/* Populate + HAL-init the I2C3 handle (Instance I2C3, 100 kHz, 7-bit). OEM 0x0803C660. */
void i2c3_handle_init(void);

#endif
