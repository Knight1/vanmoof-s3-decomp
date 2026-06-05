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

#endif
