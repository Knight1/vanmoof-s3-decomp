#ifndef MAINWARE_SENSOR_H
#define MAINWARE_SENSOR_H

#include <stdint.h>

/* Read the supply/battery voltage: raw ADC sample -> fixed-point scale ->
 * 10-sample moving-average filter; returns the smoothed reading. OEM
 * supply_voltage_read at 0x08032D6C (used by the volume brownout limiter and
 * the periodic sampler). */
uint16_t supply_voltage_read(void);

#endif
