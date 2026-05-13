/* gpio.c — port clocking and pin configuration helpers. */

#include "gpio.h"
#include "mm32f031.h"

void gpio_port_clock_enable(const gpio_t *port)
{
    uint32_t bit = 0u;
    if      (port == GPIOA) bit = RCC_AHBENR_IOPAEN_Msk;
    else if (port == GPIOB) bit = RCC_AHBENR_IOPBEN_Msk;
    else if (port == GPIOC) bit = RCC_AHBENR_IOPCEN_Msk;
    else if (port == GPIOD) bit = RCC_AHBENR_IOPDEN_Msk;
    else if (port == GPIOF) bit = RCC_AHBENR_IOPFEN_Msk;
    if (bit != 0u) {
        RCC->AHBENR |= bit;
    }
}

void gpio_pin_mode(gpio_t *port, uint8_t pin, gpio_mode_t mode)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->MODER;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)mode & 0x3u) << shift;
    port->MODER = v;
}

void gpio_pin_output_type(gpio_t *port, uint8_t pin, gpio_otype_t ot)
{
    const uint32_t bit = 1u << (pin & 0xFu);
    if (ot == GPIO_OTYPE_OD) {
        port->OTYPER |= bit;
    } else {
        port->OTYPER &= ~bit;
    }
}

void gpio_pin_speed(gpio_t *port, uint8_t pin, gpio_speed_t sp)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->OSPEEDR;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)sp & 0x3u) << shift;
    port->OSPEEDR = v;
}

void gpio_pin_pull(gpio_t *port, uint8_t pin, gpio_pull_t pull)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->PUPDR;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)pull & 0x3u) << shift;
    port->PUPDR = v;
}

void gpio_pin_alt_func(gpio_t *port, uint8_t pin, uint8_t af)
{
    const uint8_t idx   = (uint8_t)((pin & 0xFu) >> 3);   /* 0 for pin 0-7, 1 for 8-15 */
    const uint32_t shift = (uint32_t)(pin & 0x7u) * 4u;
    uint32_t v = port->AFR[idx];
    v &= ~(0xFu << shift);
    v |= ((uint32_t)af & 0xFu) << shift;
    port->AFR[idx] = v;
}

void gpio_pin_set(gpio_t *port, uint8_t pin)
{
    port->BSRR = 1u << (pin & 0xFu);
}

void gpio_pin_clear(gpio_t *port, uint8_t pin)
{
    port->BRR = 1u << (pin & 0xFu);
}

void gpio_pin_write(gpio_t *port, uint8_t pin, bool value)
{
    if (value) {
        gpio_pin_set(port, pin);
    } else {
        gpio_pin_clear(port, pin);
    }
}

bool gpio_pin_read(const gpio_t *port, uint8_t pin)
{
    return (port->IDR & (1u << (pin & 0xFu))) != 0u;
}
