#ifndef BATTERYWARE_H
#define BATTERYWARE_H

#include <stdint.h>

/* GPIO bit write: atomic set (BSRR) or clear (BRR) */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* Busy-wait millisecond delay */
void delay_ms(uint32_t ms);

/* LED flash routine — toggles a GPIO pin with fast/slow timing */
void led_flash(void);

#endif /* BATTERYWARE_H */
