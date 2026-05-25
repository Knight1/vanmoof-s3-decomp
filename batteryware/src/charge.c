#include "batteryware.h"

/* SRAM status register — bit 6 tracks charge MOSFET state */
static volatile uint32_t * const s_mosfet_status = (volatile uint32_t *)0x20002C00;

/*
 * Charge MOSFET control via GPIOB pin 9 (bit 0x200).
 *
 * Idempotent — only toggles the GPIO if the cached state in the SRAM
 * status register differs. Sets/clears bit 6 to track current state.
 */
void charge_mosfet_set(bool on)
{
    if (on) {
        if (((*s_mosfet_status >> 6) & 1) == 0) {
            *s_mosfet_status |= 0x40;
            gpio_bit_write(0x50000400, 0x200, 1);
        }
    } else {
        if (((*s_mosfet_status >> 6) & 1) != 0) {
            *s_mosfet_status &= ~0x40U;
            gpio_bit_write(0x50000400, 0x200, 0);
        }
    }
}
