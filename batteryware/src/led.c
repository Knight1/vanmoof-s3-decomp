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
