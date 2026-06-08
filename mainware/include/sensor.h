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

#endif
