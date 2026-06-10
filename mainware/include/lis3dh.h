#ifndef MAINWARE_LIS3DH_H
#define MAINWARE_LIS3DH_H

/* ST LIS3DH 3-axis accelerometer driver (I2C3, 8-bit addr 0x33 / 7-bit 0x19).
 * Public API consumed by main.c (boot probe), states.c (motion-wake config +
 * INT1 servicing) and console.c (power-down). The register-level helpers and
 * the I2C transport live private to lis3dh.c. */

/* Bring up the device: install the I2C transport, mask the INT EXTI lines and
 * verify WHO_AM_I. Returns 0 = OK, 1 = WHO_AM_I read failed, 2 = bus wait
 * failed, 3 = wrong WHO_AM_I (not 0x33). OEM 0x0803D0BC. */
int lis3dh_accel_init(void);

/* Configure INT1 as a high-pass-filtered motion (high-event) interrupt.
 * mode 0 = OR of X/Y/Z high events; mode != 0 = X high event only. threshold =
 * INT1_THS[6:0]. OEM 0x0803D120. */
void lis3dh_config_motion_int(int mode, int threshold);

/* Power the sensor down (CTRL_REG1 ODR = 0). Returns the transport status of
 * the underlying register write. OEM 0x0803D110. */
int lis3dh_powerdown(void);

/* Drain the latched INT1 condition: spin (<=99 retries, watchdog-kicked) while
 * the INT1 GPIO stays asserted, reading INT1_SRC to clear it; log changes.
 * OEM 0x08029AB8. */
void lis3dh_int1_clear(void);

#endif
