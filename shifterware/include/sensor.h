#ifndef SHIFTER_SENSOR_H
#define SHIFTER_SENSOR_H

#include <stdint.h>
#include "shifter.h"

void     sensor_init(void);

/* Raw 12-bit ADC values. */
uint16_t sensor_hall_a_raw(void);
uint16_t sensor_hall_b_raw(void);
uint16_t sensor_vsense_raw(void);

/* Cooked readings. */
uint16_t sensor_supply_mv(void);
uint8_t  sensor_position_gear(void);    /* SHIFTER_GEAR_MIN .. SHIFTER_GEAR_MAX, or 0 if invalid */

#endif /* SHIFTER_SENSOR_H */
