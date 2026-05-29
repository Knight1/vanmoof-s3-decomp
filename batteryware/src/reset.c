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
 * System reset via SCB AIRCR with DSB barriers and NOP padding.
 *
 * Identical to nvic_system_reset but includes NOP (mov r8,r8) instructions
 * between the DSB and AIRCR write. This is the "fault" variant called from
 * error paths — the NOPs serve as alignment/padding for the exception
 * return sequence.
 */
void system_reset_fault(void)
{
    __DSB();
    __asm__ volatile("mov r8, r8");
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;
    __DSB();
    __asm__ volatile("mov r8, r8");
    __asm__ volatile("mov r8, r8");
    while (1) { }
}

/*
 * nvic_system_reset_dup / nvic_system_reset_v3 — additional copies of
 * __NVIC_SystemReset that GCC inlined-then-emitted into other translation
 * units in the OEM build (FUN_08007228 / FUN_08009AA0). Byte-identical to
 * nvic_system_reset: DSB, write VECTKEY|SYSRESETREQ to SCB->AIRCR, DSB, spin.
 * Kept as distinct symbols so their call sites (e.g. led.c) resolve.
 */
void nvic_system_reset_dup(void)
{
    __DSB();
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;
    __DSB();
    while (1) { }
}

void nvic_system_reset_v3(void)
{
    __DSB();
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;
    __DSB();
    while (1) { }
}

/* system_reset_simple (FUN_08006328) — bare wrapper that tail-calls
 * system_reset(); a separate symbol in the OEM image. */
void system_reset_simple(void)
{
    system_reset();
}

/*
 * System initialization — early boot setup.
 *
 * Calls GPIO init (FUN_08007D78), modem config, the PA10-gated service-UART
 * bring-up (service_uart_init / FUN_0800AB7C), the main clock setup
 * (FUN_08000658), and state_timer_10 (FUN_08006FBC, via irq_wait_handler).
 * Then enables NVIC IRQ 5 (RCC) and IRQ 7 (PVD) with zero priority.
 *
 * Note: the earlier source called `phase2_init` here — that was the wrong-
 * address guess for FUN_0800AB7C, which is actually service_uart_init. It
 * also spuriously ran state_timer_10 twice (phase2_init + irq_wait_handler).
 */
void system_init(void)
{
    extern void gpio_init_buttons(void);  /* FUN_08007d78 */
    extern void main_clock_setup(void);  /* FUN_08000658 */
    extern void irq_wait_handler(void);  /* FUN_08006fbc = state_timer_10 */

    gpio_init_buttons();
    modem_config();
    service_uart_init();
    main_clock_setup();
    irq_wait_handler();

    flash_opt_byte_op(5, 0);
    nvic_enable_irq_s(5);
    flash_opt_byte_op(7, 0);
    nvic_enable_irq_s(7);
}
