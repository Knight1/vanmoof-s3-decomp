#include <stdint.h>

#include "systick.h"

volatile uint32_t g_systick_counter;
volatile uint8_t  g_systick_step = 1u;

void systick_tick(void)
{
    g_systick_counter += g_systick_step;
}
