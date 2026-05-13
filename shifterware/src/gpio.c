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

/* ---- OEM-confirmed GPIO helpers ----------------------------------- */

/* OEM @ 0x08004DBC (20 B). Test masked bits in the IDR of an
 * STM32F1-style GPIO port. Accesses the register by raw byte offset
 * (0x08) to bypass the speculative `gpio_t` struct, whose IDR field
 * is laid out at the STM32F0 offset (0x10) and so would point at the
 * wrong register on this MCU. Plan-2 will fix `gpio_t` itself. */
bool gpio_idr_test(void *port, uint32_t mask)
{
    return (*(volatile uint32_t *)((char *)port + 0x08u) & mask) != 0u;
}

/* OEM @ 0x0800325C (22 B). True iff GPIOA pin 0 reads high. */
bool input_pa0(void)
{
    return gpio_idr_test((void *)0x48000000u, 0x1u);
}

/* OEM @ 0x080033CC (22 B). True iff GPIOA pin 1 reads high. */
bool input_pa1(void)
{
    return gpio_idr_test((void *)0x48000000u, 0x2u);
}

/* OEM @ 0x08004DF4 (4 B). Raw BSRR write (sets bits at offset 0x10). */
void gpio_bsrr_write(void *port, uint32_t mask)
{
    *(volatile uint32_t *)((char *)port + 0x10u) = mask;
}

/* OEM @ 0x08004DF8 (4 B). Raw BRR write (clears bits at offset 0x14). */
void gpio_brr_write(void *port, uint32_t mask)
{
    *(volatile uint32_t *)((char *)port + 0x14u) = mask;
}
