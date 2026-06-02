#include "powerbankware.h"

/*
 * timer_start_it — OEM FUN_0801CF70.
 *
 * Start a timer behind a HAL handle: the handle's first member is the
 * peripheral base, +0x0C is DIER (set UIE) and +0x00 is CR1 (set CEN). Used by
 * the state super-loop to arm the periodic interrupt. Returns 0 (OEM keeps the
 * HAL_OK convention).
 */
uint32_t timer_start_it(uint32_t *handle)
{
    volatile uint32_t *base = *(volatile uint32_t **)handle;
    base[3] |= 1u;   /* DIER (+0x0C): UIE  */
    base[0] |= 1u;   /* CR1  (+0x00): CEN  */
    return 0;
}

extern int FUN_0801cf08(void *htim);                 /* HAL_TIM_Base_Init */
extern int FUN_0801d0fc(void *htim, void *master);   /* HAL_TIMEx_MasterConfigSynchronization */

/*
 * tim7_init — OEM FUN_0800e910. One of board_init's peripheral sub-inits.
 * Bring up TIM7 as the periodic-interrupt timebase (PSC=0, up-count, ARR=0x6820,
 * auto-reload-preload on) and enable its IRQ (18 = TIM7_IRQn). HAL handle @
 * 0x20000738 (armed later by timer_start_it). Field values disasm-confirmed.
 */
void tim7_init(void)
{
    volatile uint32_t * const rcc_apb1enr = (volatile uint32_t *)(0x40021000 + 0x1c);
    uint32_t * const htim = (uint32_t *)0x20000738u;
    uint32_t master[2];
    mem_set(master, 0, sizeof master);

    *rcc_apb1enr |= 0x20u;  (void)(*rcc_apb1enr & 0x20u);   /* TIM7EN + read-back */

    htim[0] = 0x40001400u;   /* Instance = TIM7              */
    htim[1] = 0;             /* Init.Prescaler              */
    htim[2] = 0;             /* Init.CounterMode = UP       */
    htim[3] = 0x6820;        /* Init.Period (ARR)           */
    htim[4] = 0;             /* Init.ClockDivision          */
    htim[6] = 0x80;          /* Init.AutoReloadPreload (+0x18) */

    if (FUN_0801cf08(htim) != 0) { spi_error_reset(); }

    master[0] = 0;           /* MasterOutputTrigger */
    master[1] = 0;           /* MasterSlaveMode     */
    if (FUN_0801d0fc(htim, master) != 0) { spi_error_reset(); }

    nvic_set_priority(18, 0, 0);   /* TIM7_IRQn */
    nvic_enable_irq(18);
}
