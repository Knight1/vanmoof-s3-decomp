#ifndef MAINWARE_SENSOR_H
#define MAINWARE_SENSOR_H

#include <stdint.h>

/* Read the supply/battery voltage: raw ADC sample -> fixed-point scale ->
 * 10-sample moving-average filter; returns the smoothed reading. OEM
 * supply_voltage_read at 0x08032D6C (used by the volume brownout limiter and
 * the periodic sampler). */
uint16_t supply_voltage_read(void);

/* 10-sample circular moving-average filter (OEM 0x08032AB0). Pushes a sample
 * into the ring buffer shared with the ADC context and returns the mean. */
uint16_t moving_avg10_push(uint16_t sample);

/* Charger-level byte for BLE 0x5543: charger ADC sample/10, or 0xFF when the
 * 12-bit sample reads full-scale (0xFFF = invalid). OEM 0x08037160. */
uint8_t charge_level_adc_get(void);

/* Identify the board HW revision 0..15 from the HW-ID divider ADC voltage vs a
 * 16-entry flash table; fills *out, returns 1 on match else 0. OEM 0x08032ce4. */
int hw_version_lookup(uint8_t *out);

/* STC3115 LiPo fuel-gauge: enter run mode (set MODE register bit 0). OEM 0x080398B8. */
void stc_gas_gauge_set_run(void);

/* HDC1080 temperature/humidity (I2C device 0x80). */
int  hdc1080_set_pointer(void *hi2c);                              /* 0x08033164 (HAL status) */
int  hdc1080_read(void *hi2c, int16_t *temp_dC, uint16_t *rh_pct); /* 0x08033188 */

/* 6-sample min/max-reject mean filter (ring at 0x20006E48). OEM 0x08038ED4. */
uint32_t sensor_filter6_push(uint16_t sample);

/* Latch the live ADC sampling config (+0x18/+0x1c/+0x20) into its shadow
 * (+0x24/+0x28/+0x2c) while the ADC status byte +0x22 is clear. OEM 0x08032CBC. */
void adc_config_shadow_copy(void);

/* Wheel-speed capture (state block 0x20006E48; EXTI9_5 pulse + TIM7 period). */
void speed_capture_init(void *cfg_a, void *cfg_b);   /* 0x08038F30 */
void tim7_app_hook(void);                             /* 0x08039138 */
void exti9_5_app_hook(void);                          /* 0x08038FF4 */

#endif
