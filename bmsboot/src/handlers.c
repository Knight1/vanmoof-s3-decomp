/* handlers.c — exception / interrupt handlers + reset path.
 *
 * Reconstructed from HardFault_Handler (0x08000C50), failsafe (0x08000C5E),
 * system_reset (0x0800095C) and SysTick_Handler (0x0800169C).
 *
 * The loader has no STL self-test suite (unlike the STM32F0 sibling
 * powerbankboot): the only live fault handler is HardFault, which drops into
 * the fail-safe and resets the chip. NMI / SVCall / PendSV are left pointing at
 * Default_Handler by the OEM vector table. SysTick paces the super-loop by
 * posting sub-rate event bits.
 */
#include "bmsboot.h"

/* CMSIS data-synchronisation barrier (the OEM emits DSB around the reset). */
static inline void dsb(void) { __asm volatile ("dsb 0xf" ::: "memory"); }

/* system_reset() — SCB_AIRCR SYSRESETREQ, then spin until the reset lands.
 * This is CMSIS NVIC_SystemReset(). */
void system_reset(void)
{
    dsb();
    REG32(SCB_BASE + 0x0C) = AIRCR_SYSRESET;   /* SCB->AIRCR = 0x05FA0004 */
    dsb();
    for (;;)
        ;
}

/* failsafe() — flash-failure / fault trap. The OEM simply resets. */
void failsafe(void)
{
    system_reset();
}

void HardFault_Handler(void)
{
    failsafe();          /* reset on hard fault (no return) */
}

/* ---- SysTick: pace the super-loop ----
 * A free-running millisecond counter plus a 0..999 sub-divider. Each tick posts
 * event bits whose set widens at coarser sub-rate boundaries (every 10/50/100/
 * 250/500 ticks and at the 1000-tick rollover). main() consumes g_boot_events to
 * run its boot-decision, watchdog-kick and download-service steps. */
volatile uint32_t g_systick_ms;        /* 0x20001D50 — free-running ms tick   */
static   uint16_t s_subtick;           /* 0x200008B8 — 0..999 sub-divider     */

void SysTick_Handler(void)
{
    g_systick_ms++;

    if (++s_subtick < 1000u) {
        if      (s_subtick % 500u == 0) g_boot_events |= 0x3Fu;
        else if (s_subtick % 250u == 0) g_boot_events |= 0x17u;
        else if (s_subtick % 100u == 0) g_boot_events |= 0x0Fu;
        else if (s_subtick %  50u == 0) g_boot_events |= 0x07u;
        else if (s_subtick %  10u == 0) g_boot_events |= 0x03u;
        else                            g_boot_events |= 0x01u;
    } else {
        s_subtick = 0;
        g_boot_events |= 0x7Fu;
    }
}
