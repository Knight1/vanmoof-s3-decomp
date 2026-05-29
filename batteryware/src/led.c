#include "batteryware.h"

/* Pointer to SRAM boolean controlling LED flash speed */
static uint8_t * const s_p_fast_mode = (uint8_t *)0x20002BFC;

/*
 * LED flash routine.
 *
 * Sets a GPIO pin at 0x50000000 (GPIOA bit 4) high, waits for a delay
 * that varies based on s_p_fast_mode (100ms or 20ms), then clears the
 * pin and waits again (50ms or 10ms).
 */
void led_flash(void)
{
    gpio_bit_write(0x50000000, 0x10, 1);
    if (*s_p_fast_mode == 0) {
        delay_ms(100);
    } else {
        delay_ms(20);
    }
    gpio_bit_write(0x50000000, 0x10, 0);
    if (*s_p_fast_mode == 0) {
        delay_ms(50);
    } else {
        delay_ms(10);
    }
}

/*
 * Fault recovery / reset path (name is a decomp guess — NOT an LED).
 *
 * Writes magic value to SRAM, calls nvic_system_reset_dup, clears
 * fault timer, sets RCC bit 0x400, pulses GPIOA pin 2 high.
 */
void fault_led_trigger(void)
{
    volatile uint32_t * const s_magic = (volatile uint32_t *)0x20002C2C;
    volatile uint32_t * const s_timer = (volatile uint32_t *)0x20002C44;
    volatile uint32_t * const s_rcc   = (volatile uint32_t *)0x20002C48;

    *s_magic = 0x05FA0004;
    nvic_system_reset_dup();
    *s_timer = 0;
    *s_rcc |= 0x400;
    gpio_bit_write(0x50000400, 2, 1);
}
