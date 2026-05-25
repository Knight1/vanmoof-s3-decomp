#include "batteryware.h"
#include <stdbool.h>

/*
 * GPIO atomic bit write using BSRR (set) and BRR (reset) registers.
 *
 * On STM32L0:
 *   GPIOx_BSRR (offset 0x18) — writing 1 to bit[N] sets pin N
 *   GPIOx_BRR  (offset 0x28) — writing 1 to bit[N] resets pin N
 *
 * Both registers are write-only and atomic — no read-modify-write needed.
 */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value)
{
    if (value == 0) {
        *(volatile uint32_t *)(gpio_base + 0x28) = (uint32_t)pin_bit;
    } else {
        *(volatile uint32_t *)(gpio_base + 0x18) = (uint32_t)pin_bit;
    }
}

/*
 * GPIO input read via IDR register (offset 0x10).
 * Returns true if the pin is high.
 */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit)
{
    return (*(volatile uint32_t *)(gpio_base + 0x10) & (uint32_t)pin_bit) != 0;
}

/*
 * GPIO pin reset — reset mode for multiple pins.
 *
 * Iterates through each bit in pin_mask. For each bit set:
 *   1. Maps gpio_base to a port index (0-5 for GPIOA-F, 6=other)
 *   2. If the port matches in the MODER register, clears BSRR/BRR
 *      output registers and sets the MODER field to 0 (input mode)
 *   3. Configures OSPEEDR, PUPDR, and OTYPER for the pin
 *
 * Used to deconfigure GPIO pins after USART/modem operations.
 */
void gpio_pin_reset(uint32_t *gpio_base, uint32_t pin_mask)
{
    volatile uint32_t * const s_mode_reg  = (volatile uint32_t *)0x200024FC;
    uint32_t i;

    for (i = 0; pin_mask >> (i & 0xFF) != 0; i++) {
        uint32_t bit = pin_mask & (1U << (i & 0xFF));
        if (bit == 0) continue;

        int port_idx;
        if (gpio_base == (uint32_t *)0x50000000) {
            port_idx = 0;
        } else if (gpio_base == (uint32_t *)0x50000400) {
            port_idx = 1;
        } else if (gpio_base == (uint32_t *)0x50000800) {
            port_idx = 2;
        } else if (gpio_base == (uint32_t *)0x50000C00) {
            port_idx = 3;
        } else if (gpio_base == (uint32_t *)0x50001000) {
            port_idx = 4;
        } else if (gpio_base == (uint32_t *)0x50001400) {
            port_idx = 5;
        } else {
            port_idx = 6;
        }

        volatile uint32_t *mode = (volatile uint32_t *)(s_mode_reg[(i >> 2) + 2]);
        uint32_t shift = (i & 3) << 2;
        if ((mode[0] & (0xFU << shift)) == ((uint32_t)port_idx << shift)) {
            gpio_base[6] &= ~bit;   /* BSRR */
            gpio_base[7] &= ~bit;   /* BRR */
            gpio_base[8] &= ~bit;   /* clear output */
            gpio_base[9] &= ~bit;
            mode[0] &= ~(0xFU << shift);
        }

        gpio_base[0] |= 3U << ((i & 0x7F) << 1);         /* OSPEEDR */
        gpio_base[(i >> 3) + 8] &= ~(0xFU << ((i & 7) << 2));  /* AFR */
        gpio_base[3] &= ~(3U << ((i & 0x7F) << 1));       /* PUPDR */
        gpio_base[1] &= ~(1U << (i & 0xFF));               /* OTYPER */
        gpio_base[2] &= ~(3U << ((i & 0x7F) << 1));       /* MODER */
    }
}
