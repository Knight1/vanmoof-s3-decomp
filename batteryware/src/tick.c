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

/* Additional tick counter at SRAM 0x200047DC */
static volatile uint32_t * const s_tick2 = (volatile uint32_t *)0x200047DC;

/*
 * Read the secondary system tick counter.
 *
 * Used pervasively as the general-purpose tick reference
 * (timers, flash operations, DMA polls).
 */
uint32_t tick_get(void)
{
    return *s_tick2;
}

/* Raw tick counter at SRAM 0x200000C8 */
static volatile uint32_t * const s_tick_raw = (volatile uint32_t *)0x200000C8;

/*
 * Read the raw hardware tick counter.
 *
 * Used by clock-prescaler-dependent functions (fg_read_field_8/11)
 * to determine APB prescaler scaling factors.
 */
uint32_t tick_counter_read(void)
{
    return *s_tick_raw;
}
