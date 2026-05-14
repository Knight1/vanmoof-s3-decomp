#include <stdint.h>

#include "systick.h"

volatile uint32_t g_systick_counter;
volatile uint8_t  g_systick_step = 1u;

void systick_tick(void)
{
    g_systick_counter += g_systick_step;
}

uint32_t systick_get_count(void)
{
    return g_systick_counter;
}

void systick_delay(uint32_t ticks)
{
    uint32_t start = systick_get_count();

    if (ticks != 0xFFFFFFFFu) {
        ticks += g_systick_step;
    }

    while ((systick_get_count() - start) < ticks) {
        /* busy wait */
    }
}
