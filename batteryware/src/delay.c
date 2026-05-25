#include "batteryware.h"

/* System tick control register — polled for delay timing */
static volatile uint32_t * const s_systick_ctrl = (volatile uint32_t *)0x20002C80;
/* SRAM variable written by the timer IRQ handler */
static volatile uint32_t * const s_systick_flag = (volatile uint32_t *)0x20002C10;
static const uint32_t s_systick_reload = 0x0000AAAA;

/*
 * Busy-wait millisecond delay using SysTick timer polling.
 *
 * Waits for the timer to fire 'ms' times. Each iteration polls the
 * SysTick count-flag (bit 16 of CTRL) and clears it when set.
 */
void delay_ms(uint32_t ms)
{
    for (uint32_t i = ms; i != 0; i--) {
        /* Wait for COUNTFLAG to be set */
        while ((*s_systick_ctrl & 1) == 0) { }
        /* Clear COUNTFLAG */
        *s_systick_ctrl = *s_systick_ctrl & ~1U;

        /* If overflow flag is set (bit 1), clear it and reload */
        if ((*s_systick_ctrl & 2) != 0) {
            *s_systick_ctrl = *s_systick_ctrl & ~2U;
            *s_systick_flag = s_systick_reload;
        }
    }
}

/*
 * Busy-wait microsecond delay.
 *
 * Reads a hardware counter from SRAM, divides by 1,000,000 to get
 * loop iterations per microsecond, multiplies by requested us, and
 * spins for that many iterations.
 */
void delay_us(uint32_t us)
{
    volatile uint32_t * const s_counter = (volatile uint32_t *)0x200000C8;
    uint32_t cycles = *s_counter / 1000000;
    uint32_t count = cycles * us;

    for (volatile uint32_t i = count; i != 0; i--) {
    }
}
