#ifndef MAINWARE_LIGHTING_H
#define MAINWARE_LIGHTING_H

#include <stdint.h>

/* Front/rear lamp engine + ambient light sensor (src/lighting.c). The three
 * lamp channels are TIM PWM outputs (CCR1/CCR2/CCR3); the animation engine fades
 * each toward a target brightness and runs table-driven flash patterns. The
 * target is chosen from the light mode (auto/on/off), the ambient-light sensor,
 * and the power state. See docs/lighting.md. Channels live in g_lights
 * @ 0x20006DC0; the sensor object @ 0x20000090. */

/* Per-tick lamp animation step for one channel (called 3x from the super-loop,
 * channel 0/1/2). trigger -> pending request byte (consumed); threshold ->
 * ambient-light on/off threshold; mode -> 0 auto / 1 on / 2 off. */
void light_pattern_step(uint8_t *trigger, int channel, uint32_t threshold, int mode);
/* Apply one pattern step action (0..6): set brightness target / schedule a fade. */
void light_pattern_action_apply(uint8_t action, uint8_t *level, uint8_t *ch);

/* Ambient light sensor: poll-throttled I2C read; caches the 16-bit reading and
 * manages the sensor-fault flag. Returns the cached lux value. */
uint16_t light_sensor_read_step(void);
/* One ambient-sensor I2C transaction (write reg 0x50, read 2 bytes). Returns the
 * HAL status (2 if the bus is busy); the little-endian reading lands in *out. */
uint8_t  light_sensor_i2c_read(uint16_t *out);
/* Read the sensor-retry/fault counter byte. */
uint8_t  light_sensor_fault_count_get(void);

/* Lamp PWM brightness setters = the three channel callbacks (TIM CCR1/2/3). */
void obj_set_field34(uint32_t duty);            /* CCR1 */
void obj_set_field38(uint32_t duty);            /* CCR2 */
void led_channel3_set_brightness(uint32_t duty);/* CCR3 */

#endif
