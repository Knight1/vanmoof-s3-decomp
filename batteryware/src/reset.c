#include "batteryware.h"

/*
 * System reset using the Cortex-M0+ SCB AIRCR register.
 *
 * Sequence:
 *   1. DSB — ensures all memory accesses complete
 *   2. Write VECTKEY (0x05FA) + SYSRESETREQ (bit 2) to AIRCR (0xE000ED0C)
 *   3. DSB — ensures the write takes effect
 *   4. Infinite loop — waits for the reset to trigger
 *
 * CMSIS-equivalent: __NVIC_SystemReset()
 */
void nvic_system_reset(void)
{
    __DSB();
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;
    __DSB();
    while (1) { }
}

void system_reset(void)
{
    nvic_system_reset();
}

void system_reset_with_arg(uint32_t arg)
{
    (void)arg;
    system_reset();
}
