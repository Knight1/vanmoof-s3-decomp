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

/* hal_tim_msp_init — OEM FUN_0801cf60 (HAL_TIM_Base_MspInit): empty weak callback
 * (the TIM7 clock is enabled directly in tim7_init). */
void hal_tim_msp_init(void *htim)
{
    (void)htim;
}
extern void FUN_0801cff8(volatile uint32_t *inst, const uint32_t *init); /* TIM_Base_SetConfig (own pass) */

/*
 * hal_tim_base_init — OEM FUN_0801cf08 (HAL_TIM_Base_Init).
 * MSP init, then TIM_Base_SetConfig(Instance, &Init) (which programs CR1/ARR/
 * PSC + an update event). Lock +0x3c, State +0x3d. Init begins at handle +0x04.
 */
int hal_tim_base_init(void *handle)
{
    if (handle == NULL) {
        return 1;
    }
    uint8_t *h = (uint8_t *)handle;

    if (h[0x3d] == 0) {                             /* State == RESET */
        h[0x3c] = 0;                                /* Lock = UNLOCKED */
        hal_tim_msp_init(handle);                   /* HAL_TIM_Base_MspInit */
    }
    h[0x3d] = 2;                                    /* State = BUSY */

    volatile uint32_t *inst = *(volatile uint32_t **)h;   /* Instance */
    FUN_0801cff8(inst, (const uint32_t *)(h + 4));        /* TIM_Base_SetConfig(&Init) */

    h[0x3d] = 1;                                    /* State = READY */
    return 0;
}

/*
 * hal_timex_master_config — OEM FUN_0801d0fc
 * (HAL_TIMEx_MasterConfigSynchronization). Program CR2.MMS (+0x04, bits 6:4)
 * := MasterOutputTrigger and SMCR.MSM (+0x08, bit 7) := MasterSlaveMode, each as
 * the OEM's separate mask-then-OR store pair. Lock +0x3c. cfg = {MMS, MSM}.
 */
int hal_timex_master_config(void *handle, const uint32_t *cfg)
{
    uint8_t *h = (uint8_t *)handle;

    if (h[0x3c] == 1) {                             /* Lock held */
        return 2;                                   /* HAL_BUSY */
    }
    h[0x3c] = 1;                                    /* Lock */
    h[0x3d] = 2;                                    /* State = BUSY */

    volatile uint32_t *inst = *(volatile uint32_t **)h;   /* Instance */
    inst[1] &= 0xffffff8fu;  inst[1] |= cfg[0];     /* CR2  MMS  := MasterOutputTrigger */
    inst[2] &= 0xffffff7fu;  inst[2] |= cfg[1];     /* SMCR MSM  := MasterSlaveMode      */

    h[0x3c] = 0;                                    /* Unlock */
    h[0x3d] = 1;                                    /* State = READY */
    return 0;
}

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

    if (hal_tim_base_init(htim) != 0) { spi_error_reset(); }

    master[0] = 0;           /* MasterOutputTrigger */
    master[1] = 0;           /* MasterSlaveMode     */
    if (hal_timex_master_config(htim, master) != 0) { spi_error_reset(); }

    nvic_set_priority(18, 0, 0);   /* TIM7_IRQn */
    nvic_enable_irq(18);
}
