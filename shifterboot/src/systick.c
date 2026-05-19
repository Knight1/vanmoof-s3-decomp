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

/* OEM @ 0x080014CA (20 B). Single-arg millisecond delay.
 *
 * The OEM body spills the `ms` arg to the stack on entry
 * (`push {r0, lr}; ldr r0, [sp, #0]`) and uses `pop {r3, pc}` on
 * exit — a `-O0`-style "every parameter is stack-resident" pattern;
 * the popped `r3` is discarded. We translate the visible
 * semantics; gcc `-Os` keeps `ms` in a register.
 *
 * The `nop` at PC `0x080014D2` between the store and the spin is a
 * 2-byte alignment pad for the literal-pool fetch that follows. */
void mdelay(uint32_t ms)
{
    g_systick_countdown = ms;
    while (g_systick_countdown != 0u) {
        /* spin until SysTick_Handler ticks the counter down to 0 */
    }
}
