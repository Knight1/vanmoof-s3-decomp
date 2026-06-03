#include "powerbankware.h"

/* TIM instance bases (APB literal-pool values from the OEM image). */
#define TIM1_BASE   0x40012c00u
#define TIM2_BASE   0x40000000u
#define TIM3_BASE   0x40000400u
#define TIM7_BASE   0x40001400u
#define TIM14_BASE  0x40002000u
#define TIM15_BASE  0x40014000u
#define TIM16_BASE  0x40014400u
#define TIM17_BASE  0x40014800u

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

    if (base == TIM1_BASE || base == TIM2_BASE || base == TIM3_BASE) {   /* TIM1/2/3 */
        cr1 = init[1] | (cr1 & 0xffffff8fu);            /* CounterMode (CMS|DIR) */
    }
    if (base == TIM1_BASE || base == TIM2_BASE || base == TIM3_BASE ||
        base == TIM14_BASE || base == TIM15_BASE || base == TIM16_BASE ||
        base == TIM17_BASE) {                           /* TIM1/2/3/14/15/16/17 */
        cr1 = init[3] | (cr1 & 0xfffffcffu);            /* ClockDivision (CKD) */
    }
    inst[0]   = init[5] | (cr1 & 0xffffff7fu);          /* CR1: AutoReloadPreload (ARPE) */
    inst[0xb] = init[2];                                 /* ARR = Period    */
    inst[10]  = init[0];                                 /* PSC = Prescaler */
    if (base == TIM1_BASE || base == TIM15_BASE ||
        base == TIM16_BASE || base == TIM17_BASE) {     /* TIM1/15/16/17 */
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

    htim[0] = TIM7_BASE;     /* Instance = TIM7              */
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

/*
 * TIM7_IRQHandler — OEM FUN_0800e438 (TIM7 period-elapsed ISR, 384 bytes).
 *
 * Hand-written interrupt handler (NOT the generic HAL_TIM_IRQHandler): it
 * checks SR.UIF & DIER.UIE, clears UIF, then drives a PA9 indicator
 * (LED/buzzer) bit-pattern state machine off the TIM7 timebase.
 *
 * The state machine has two layers, gated by flagsA bit5 (0x20):
 *   - bit5 CLEAR -> step the high-level "substate" sequencer via a 10-way
 *     switch on `substate` (table @ 0x0801e5f8, all targets internal to this
 *     ISR). Every case down-counts the u16 `divcnt` and only acts when it
 *     reaches 0. Even cases (2/4/6/8) arm the bit machine (set bit5, position
 *     the pattern, extend its length) and emit the first bit; odd cases
 *     (1/3/5/7) reload divcnt and advance the substate while pulsing PA9 high;
 *     case 9 stops the timer IT and flags completion (bit3).
 *   - bit5 SET -> shift the per-bit pattern out of arr[pos] onto PA9. flagsA
 *     bit2 (0x4) alternates between the "drive" phase (writes the current bit)
 *     and the "complement/advance" phase (writes the inverse, shifts in the
 *     next bit, advancing pos every 8 bits). When pos reaches `patlen`, clear
 *     bit5, drop PA9, reload divcnt=4 and advance the substate.
 *
 * Indicator pin is PA9 (GPIOA @ 0x48000000, bit 0x200). State lives in the
 * SRAM block at 0x20000480.. (disasm-confirmed addresses/widths/masks).
 */

/* TIM7 HAL handle — same instance armed by tim7_init(). The struct lives at
 * 0x20000738 and its first word holds the TIM7 base (0x40001400). The HAL
 * helpers (timer_start_it / tim_base_stop_it) take the STRUCT ADDRESS and
 * dereference it themselves; the ISR body wants the register block directly. */
#define TIM7_HANDLE     ((void *)0x20000738u)                 /* &htim         */
#define TIM7_HTIM7      (*(volatile uint32_t **)0x20000738u)  /* htim->Instance */

/* PA9 indicator (LED/buzzer). */
#define IND_GPIO_BASE   0x48000000u
#define IND_PIN_BIT     0x200u            /* PA9 */

/* Indicator bit-pattern machine state (SRAM @ 0x20000480..). */
#define IND_PATLEN      (*(volatile uint8_t  *)0x20000480u)   /* pattern length    */
#define IND_FLAGS       (*(volatile uint8_t  *)0x20000484u)   /* phase/run flags   */
#define IND_DIVCNT      (*(volatile uint16_t *)0x20000486u)   /* substate divider  */
#define IND_SUBSTATE    (*(volatile uint8_t  *)0x20000488u)   /* substate index    */
#define IND_POS         (*(volatile uint8_t  *)0x20000489u)   /* pattern byte pos  */
#define IND_BITCNT      (*(volatile uint8_t  *)0x2000048Au)   /* bit-in-byte count */
#define IND_ARR         ((volatile uint8_t   *)0x2000048Cu)   /* pattern bytes     */

#define IND_F_PHASE     0x04u   /* bit2: which half of the bit period         */
#define IND_F_RUN       0x20u   /* bit5: bit machine active (else substate)   */

void TIM7_IRQHandler(void)
{
    volatile uint32_t *inst = TIM7_HTIM7;          /* TIM7 registers */

    if ((inst[4] & 1u) != 1u)      /* SR  (+0x10): UIF  */
        return;
    if ((inst[3] & 1u) != 1u)      /* DIER (+0x0C): UIE */
        return;

    inst[4] = 0xfffffffeu;         /* clear UIF (rc_w0) */

    if (IND_FLAGS & IND_F_RUN) {
        /* ---- bit-pattern machine (flagsA bit5 set) ---- */
        if (IND_POS >= IND_PATLEN) {
            if (IND_POS == IND_PATLEN) {
                IND_FLAGS &= 0xdfu;                /* clear bit5 */
                gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, 0);
                IND_DIVCNT = 4;
                IND_SUBSTATE = (uint8_t)(IND_SUBSTATE + 1);
            }
            return;
        }

        uint8_t pattern = IND_ARR[IND_POS];

        if ((IND_FLAGS & IND_F_PHASE) == 0) {
            gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (pattern & 1u) ? 1 : 0);
            IND_FLAGS |= IND_F_PHASE;
        } else {
            gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (pattern & 1u) ? 0 : 1);
            IND_FLAGS &= (uint8_t)~IND_F_PHASE;    /* bics 4 */

            IND_BITCNT = (uint8_t)(IND_BITCNT + 1);
            if (IND_BITCNT > 7) {
                IND_POS = (uint8_t)(IND_POS + 1);
                IND_BITCNT = 0;
            } else {
                IND_ARR[IND_POS] = (uint8_t)(IND_ARR[IND_POS] >> 1);
            }
        }
        return;
    }

    /* ---- substate sequencer (flagsA bit5 clear) ---- */
    if (IND_SUBSTATE > 9)
        return;

    switch (IND_SUBSTATE) {
    case 1:
    case 3:
        if (--IND_DIVCNT != 0) return;
        IND_DIVCNT = 4;
        IND_SUBSTATE = (uint8_t)(IND_SUBSTATE + 1);
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, 1);
        return;

    case 5:
    case 7:
        if (--IND_DIVCNT != 0) return;
        IND_SUBSTATE = (uint8_t)(IND_SUBSTATE + 1);
        IND_DIVCNT = 4;
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, 1);
        return;

    case 2:
        if (--IND_DIVCNT != 0) return;
        IND_BITCNT = 0;
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (IND_ARR[IND_POS] & 1u) ? 1 : 0);
        IND_FLAGS |= IND_F_PHASE;                  /* set bit2 */
        IND_FLAGS |= IND_F_RUN;                     /* set bit5 */
        return;

    case 4:
        if (--IND_DIVCNT != 0) return;
        IND_BITCNT = 0;
        IND_PATLEN = (uint8_t)(IND_PATLEN + 6);
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (IND_ARR[IND_POS] & 1u) ? 1 : 0);
        IND_FLAGS |= IND_F_PHASE;
        IND_FLAGS |= IND_F_RUN;
        return;

    case 6:
        if (--IND_DIVCNT != 0) return;
        IND_BITCNT = 0;
        IND_PATLEN = (uint8_t)(IND_PATLEN + 4);
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (IND_ARR[IND_POS] & 1u) ? 1 : 0);
        IND_FLAGS |= IND_F_PHASE;
        IND_FLAGS |= IND_F_RUN;
        return;

    case 8:
        if (--IND_DIVCNT != 0) return;
        IND_BITCNT = 0;
        IND_PATLEN = (uint8_t)(IND_PATLEN + 7);
        gpio_bit_write(IND_GPIO_BASE, IND_PIN_BIT, (IND_ARR[IND_POS] & 1u) ? 1 : 0);
        IND_FLAGS |= IND_F_PHASE;
        IND_FLAGS |= IND_F_RUN;
        return;

    case 9:
        if (--IND_DIVCNT != 0) return;
        tim_base_stop_it(TIM7_HANDLE);              /* HAL_TIM_Base_Stop_IT(&htim) */
        IND_DIVCNT = 0x32;
        IND_FLAGS |= 8u;                            /* flag completion (bit3) */
        return;

    default:
        return;                                     /* case 0 / table slot 0 */
    }
}

/*
 * tim_base_stop_it — OEM FUN_0801cfa4 (HAL_TIM_Base_Stop_IT).
 * Takes the HAL handle STRUCT pointer (handle->Instance = *handle). Clear
 * DIER.UIE (+0x0C bit0); if no CC interrupt/overcapture sources remain
 * pending in +0x20 (& 0x1111 / & 0x444), clear CR1.CEN (+0x00 bit0).
 * Returns HAL_OK (0). Masks/offsets disasm-confirmed.
 */
int tim_base_stop_it(void *handle)
{
    volatile uint32_t *inst = *(volatile uint32_t **)handle;
    inst[3] &= 0xfffffffeu;                         /* DIER (+0x0C): clear UIE */
    if ((inst[8] & 0x00001111u) == 0u &&            /* (+0x20) CCxIE  */
        (inst[8] & 0x00000444u) == 0u) {            /* (+0x20) CCxOF? */
        inst[0] &= 0xfffffffeu;                     /* CR1 (+0x00): clear CEN  */
    }
    return 0;
}
