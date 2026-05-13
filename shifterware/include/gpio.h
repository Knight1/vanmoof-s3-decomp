#ifndef SHIFTER_GPIO_H
#define SHIFTER_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "mm32f031.h"

typedef enum {
    GPIO_MODE_INPUT  = 0u,
    GPIO_MODE_OUTPUT = 1u,
    GPIO_MODE_AF     = 2u,
    GPIO_MODE_ANALOG = 3u,
} gpio_mode_t;

typedef enum {
    GPIO_OTYPE_PP = 0u,
    GPIO_OTYPE_OD = 1u,
} gpio_otype_t;

typedef enum {
    GPIO_SPEED_LOW    = 0u,
    GPIO_SPEED_MEDIUM = 1u,
    GPIO_SPEED_HIGH   = 3u,
} gpio_speed_t;

typedef enum {
    GPIO_PULL_NONE = 0u,
    GPIO_PULL_UP   = 1u,
    GPIO_PULL_DOWN = 2u,
} gpio_pull_t;

void gpio_port_clock_enable(const gpio_t *port);
void gpio_pin_mode(gpio_t *port, uint8_t pin, gpio_mode_t mode);
void gpio_pin_output_type(gpio_t *port, uint8_t pin, gpio_otype_t ot);
void gpio_pin_speed(gpio_t *port, uint8_t pin, gpio_speed_t sp);
void gpio_pin_pull(gpio_t *port, uint8_t pin, gpio_pull_t pull);
void gpio_pin_alt_func(gpio_t *port, uint8_t pin, uint8_t af);

void gpio_pin_set(gpio_t *port, uint8_t pin);
void gpio_pin_clear(gpio_t *port, uint8_t pin);
void gpio_pin_write(gpio_t *port, uint8_t pin, bool value);
bool gpio_pin_read(const gpio_t *port, uint8_t pin);

/* OEM-confirmed helpers (see gpio.c). Defined against raw register
 * offsets to bypass the speculative `gpio_t` struct. */
bool gpio_idr_test(void *port, uint32_t mask);
bool input_pa0(void);
bool input_pa1(void);

#endif /* SHIFTER_GPIO_H */
