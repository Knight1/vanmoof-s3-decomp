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

/*
 * System initialization — early boot setup.
 *
 * Calls phase 1 init (GPIO clocks), modem config, phase 2 init
 * (peripheral init), and the main clock setup (FUN_08000658).
 * Then enables NVIC IRQ 5 (RCC) and IRQ 7 (PVD) with zero priority.
 */
void system_init(void)
{
    extern void phase1_init(void);       /* FUN_08007d78 */
    extern void phase2_init(void);       /* FUN_0800ab7c */
    extern void main_clock_setup(void);  /* FUN_08000658 */
    extern void irq_wait_handler(void);  /* FUN_08006fbc */

    phase1_init();
    modem_config();
    phase2_init();
    main_clock_setup();
    irq_wait_handler();

    flash_opt_byte_op(5, 0);
    nvic_enable_irq_s(5);
    flash_opt_byte_op(7, 0);
    nvic_enable_irq_s(7);
}
