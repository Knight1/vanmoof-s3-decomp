#include <stdint.h>

#include "systick.h"

volatile uint32_t g_systick_counter;
volatile uint8_t  g_systick_step = 1u;

void systick_tick(void)
{
    g_systick_counter += g_systick_step;
}

uint32_t systick_now(void)
{
    return g_systick_counter;
}

void systick_delay(uint32_t ticks)
{
    uint32_t start = systick_now();

    /* Round up by one step so a request of N waits at least N whole ticks —
     * the tick in progress when we entered may already be partly elapsed.
     * 0xFFFFFFFF is the OEM sentinel for "wait forever". */
    if (ticks != 0xFFFFFFFFu) {
        ticks += g_systick_step;
    }

    while ((uint32_t)(systick_now() - start) < ticks) {
    }
}
