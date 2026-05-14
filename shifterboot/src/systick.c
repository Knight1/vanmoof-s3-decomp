#include <stdint.h>

#include "systick.h"

volatile uint32_t g_systick_countdown;

void systick_tick(void)
{
    if (g_systick_countdown != 0u) {
        g_systick_countdown = g_systick_countdown - 1u;
    }
}

void SysTick_Handler(void)
{
    systick_tick();
}
