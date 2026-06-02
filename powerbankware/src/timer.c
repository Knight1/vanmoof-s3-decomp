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
/*
 * tim_base_set_config — OEM FUN_0801cff8 (TIM_Base_SetConfig).
 * Program CR1 (CounterMode CMS|DIR only on TIM1/2/3; ClockDivision CKD on the
 * timers that have it; AutoReloadPreload ARPE always), ARR/PSC, the repetition
 * counter (advanced timers only), then force an update event (EGR.UG). The
 * per-instance gating mirrors the OEM's base-address comparisons; for a basic
 * timer (TIM7) only ARPE/ARR/PSC/EGR apply. init = {Prescaler, CounterMode,
 * Period, ClockDivision, RepetitionCounter, AutoReloadPreload}. CR1 inst[0],
 * EGR inst[5] (+0x14), PSC inst[10] (+0x28), ARR inst[0xb] (+0x2c), RCR inst[0xc]
 * (+0x30). Offsets, masks and instance addresses disasm-confirmed.
 */
void tim_base_set_config(volatile uint32_t *inst, const uint32_t *init)
{
    uint32_t base = (uint32_t)(uintptr_t)inst;
    uint32_t cr1 = inst[0];

    if (base == 0x40012c00u || base == 0x40000000u || base == 0x40000400u) {   /* TIM1/2/3 */
        cr1 = init[1] | (cr1 & 0xffffff8fu);            /* CounterMode (CMS|DIR) */
    }
    if (base == 0x40012c00u || base == 0x40000000u || base == 0x40000400u ||
        base == 0x40002000u || base == 0x40014000u || base == 0x40014400u ||
        base == 0x40014800u) {                          /* TIM1/2/3/14/15/16/17 */
        cr1 = init[3] | (cr1 & 0xfffffcffu);            /* ClockDivision (CKD) */
    }
    inst[0]   = init[5] | (cr1 & 0xffffff7fu);          /* CR1: AutoReloadPreload (ARPE) */
    inst[0xb] = init[2];                                 /* ARR = Period    */
    inst[10]  = init[0];                                 /* PSC = Prescaler */
    if (base == 0x40012c00u || base == 0x40014000u ||
        base == 0x40014400u || base == 0x40014800u) {   /* TIM1/15/16/17 */
        inst[0xc] = init[4];                            /* RCR = RepetitionCounter */
    }
    inst[5] = 1;                                         /* EGR = UG (generate update) */
}

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
    tim_base_set_config(inst, (const uint32_t *)(h + 4));  /* TIM_Base_SetConfig(&Init) */

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
