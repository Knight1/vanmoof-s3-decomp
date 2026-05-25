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
