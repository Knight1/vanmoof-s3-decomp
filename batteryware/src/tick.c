#include "batteryware.h"

/* System tick counter — SRAM struct at 0x200047E0, field at offset 0x14 */
static volatile uint32_t * const s_tick_counter = (volatile uint32_t *)0x200047E0;

/*
 * Read the current system tick counter (milliseconds since boot).
 * Writes the value to *out and returns 0.
 */
uint32_t get_tick_ms(uint32_t *out)
{
    *out = s_tick_counter[0x14 / 4];
    return 0;
}
