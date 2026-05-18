/* timer.c — SysTick millis counter and TIM3 step-pulse generator.
 *
 * The systick/TIM3 code below is speculative pre-decomp scaffolding;
 * the real OEM SysTick handler is `FUN_0800428E` (boot_init_periphs_a,
 * still pending). The OEM TIM HAL helpers added at the bottom — the
 * `tim_time_base_*` family and `tim2_init_periodic` — are decomp-c
 * and live behind real `FUN_*` addresses. */

#include "timer.h"
#include "hal.h"
#include "mm32f031.h"

static volatile uint32_t s_millis;

void systick_init(void)
{
    SYSTICK->LOAD = (SYSCLK_HZ / 1000u) - 1u;
    SYSTICK->VAL  = 0u;
    SYSTICK->CTRL = 0x7u;   /* CLKSOURCE=core | TICKINT | ENABLE */
}

uint32_t systick_millis(void)
{
    return s_millis;
}

void systick_delay_ms(uint32_t ms)
{
    const uint32_t start = s_millis;
    while ((s_millis - start) < ms) {
        /* spin */
    }
}

void SysTick_Handler(void)
{
    s_millis++;
}

/* TIM3 is run as a periodic timebase; each update event drives one
 * micro-step from the motor driver. */
void tim3_step_init(uint32_t step_hz)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN_Msk;

    TIM3->CR1  = 0u;
    TIM3->PSC  = 47u;        /* 48 MHz / (47+1) = 1 MHz tick */
    TIM3->DIER = TIM_DIER_UIE_Msk;

    tim3_step_set_rate(step_hz);

    TIM3->EGR = TIM_EGR_UG_Msk;
    TIM3->SR  = 0u;

    NVIC->ISER[0] = 1u << 16;     /* TIM3 IRQ = 16 */
}

void tim3_step_set_rate(uint32_t step_hz)
{
    uint32_t arr = (step_hz != 0u) ? (1000000u / step_hz) : 0xFFFFu;
    if (arr == 0u) arr = 1u;
    if (arr > 0xFFFFu) arr = 0xFFFFu;
    TIM3->ARR = arr - 1u;
}

void tim3_step_start(void)
{
    TIM3->SR  = 0u;
    TIM3->CR1 |= TIM_CR1_CEN_Msk;
}

void tim3_step_stop(void)
{
    TIM3->CR1 &= ~TIM_CR1_CEN_Msk;
}

void TIM3_IRQHandler(void)
{
    if ((TIM3->SR & TIM_SR_UIF_Msk) != 0u) {
        TIM3->SR = (uint32_t)~TIM_SR_UIF_Msk;
        motor_step_tick();
    }
}

/* ---- OEM TIM HAL --------------------------------------------------- *
 *
 * The OEM ships a thin TIM HAL ported from the ST/MM32 vendor library.
 * Each helper below corresponds to one OEM symbol. They branch on the
 * TIMx base address to special-case TIM1/TIM15/TIM16 (advanced timers
 * — they have a CR1.CKD field and a repetition counter that the
 * general-purpose timers don't). */

/* Per-target base addresses (literal pool at 0x08005754..0x08005764).
 * `TIM2` doesn't live in flash literals because the OEM materialises
 * it inline as `lsls r3, #0x1e` from `#1`, giving 0x40000000. */
#define TIM_BASE_TIM1   0x40012C00u
#define TIM_BASE_TIM2   0x40000000u
#define TIM_BASE_TIM3   0x40000400u
#define TIM_BASE_TIM15  0x40014400u
#define TIM_BASE_TIM16  0x40014800u

/* OEM mask used to preserve the non-CR1-counter-mode fields when an
 * advanced timer is being configured. Equivalent to `~(CR1.DIR_Msk |
 * CR1.CMS_Msk)` — bits 4..6 cleared, all others kept. */
#define TIM_CR1_COUNTERMODE_MSK  0xFF8Fu

/* OEM @ 0x080053E4 (18 B). Reset the time-base config struct to its
 * vendor-library defaults: prescaler 0, counter-mode 0 (up), clock-div
 * 0, period 0xFFFF, repetition-counter 0. The OEM materialises 0xFFFF
 * by loading the 0xFF8F literal pool word and adding 0x70 — saves a
 * `movs/lsls` pair. */
void tim_time_base_config_init(tim_time_base_cfg_t *cfg)
{
    cfg->period       = 0xFFFFu;        /* offset 0x08 */
    cfg->prescaler    = 0u;             /* offset 0x00 */
    cfg->clock_div    = 0u;             /* offset 0x0C */
    cfg->counter_mode = 0u;             /* offset 0x04 */
    cfg->rep_counter  = 0u;             /* offset 0x10 */
}

/* OEM @ 0x0800539A (74 B). Write the time-base config into TIMx's
 * registers. Two base-address branches:
 *  - TIM1/TIM2/TIM3 (the timers that have a CR1.CMS/DIR field):
 *      CR1 = (CR1 & 0xFF8F) | cfg->counter_mode
 *  - TIM1/TIM15/TIM16 (the advanced timers with a repetition counter):
 *      RCR = cfg->rep_counter
 * After the branches: ARR = period, PSC = prescaler, EGR = UG (1) to
 * latch the new prescaler/period into the active shadow registers. */
void tim_time_base_init(void *tim_base, const tim_time_base_cfg_t *cfg)
{
    volatile uint32_t *const tim = (volatile uint32_t *)tim_base;
    const uintptr_t base = (uintptr_t)tim_base;

    uint16_t cr1 = (uint16_t)tim[0];    /* TIMx->CR1 @ offset 0x00 */
    if (base == TIM_BASE_TIM1 || base == TIM_BASE_TIM2 || base == TIM_BASE_TIM3) {
        cr1 = (uint16_t)((cr1 & TIM_CR1_COUNTERMODE_MSK) | cfg->counter_mode);
    }
    tim[0] = cr1;

    tim[0x2C / 4] = cfg->period;        /* TIMx->ARR @ offset 0x2C */
    tim[0x28 / 4] = cfg->prescaler;     /* TIMx->PSC @ offset 0x28 */

    if (base == TIM_BASE_TIM1 || base == TIM_BASE_TIM15 || base == TIM_BASE_TIM16) {
        tim[0x30 / 4] = (uint32_t)cfg->rep_counter;  /* TIMx->RCR @ offset 0x30 */
    }
    tim[0x14 / 4] = 1u;                 /* TIMx->EGR = UG */
}

/* OEM @ 0x080059A0 (8 B). `TIMx->SR = ~flag`. SR is rc_w0 (read /
 * clear-on-write-0), so writing 0 to a bit clears it and writing 1
 * leaves it alone — `~flag` clears just the bit `flag` set. */
void tim_clear_flag(void *tim_base, uint16_t flag)
{
    volatile uint32_t *const tim = (volatile uint32_t *)tim_base;
    tim[0x10 / 4] = (uint32_t)(uint16_t)~flag;
}

/* OEM @ 0x0800596E (26 B). Toggle bits in TIMx->DIER (offset 0x0C).
 * On disable the OEM ANDs with `~mask & 0xFFFF` (the uxth keeps the
 * upper half of the register clean — DIER is a 16-bit register). */
void tim_dier_bits(void *tim_base, uint16_t mask, int enable)
{
    volatile uint32_t *const dier = (volatile uint32_t *)((uintptr_t)tim_base + 0x0Cu);
    if (enable != 0) {
        *dier = *dier | mask;
    } else {
        *dier = *dier & (uint32_t)(uint16_t)~mask;
    }
}

/* OEM @ 0x08005498 (26 B). On enable: set TIMx->CR1.CEN (bit 0). On
 * disable: clear it. The disable path uses `& 0xFFFE` (materialised as
 * `[0x0800575C] + 0x6F = 0xFF8F + 0x6F = 0xFFFE`). */
void tim_enable(void *tim_base, int enable)
{
    volatile uint32_t *const tim = (volatile uint32_t *)tim_base;
    if (enable != 0) {
        tim[0] = tim[0] | 1u;
    } else {
        tim[0] = tim[0] & 0xFFFEu;
    }
}

/* OEM @ 0x08004048 (96 B). Bring up TIM2 for a periodic update IRQ.
 * Boot-only — called once from main with `(HCLK / 100000 - 1, 99)`,
 * which on a 48 MHz core gives `(479, 99)` and an update event every
 * `(479+1)*(99+1)/48e6 = 1 ms` → a 1 kHz tick. */
void tim2_init_periodic(uint16_t prescaler, uint16_t period)
{
    rcc_apb1en_bits(1u, 1);             /* RCC->APB1ENR bit 0 = TIM2EN */

    tim_time_base_cfg_t cfg;
    tim_time_base_config_init(&cfg);

    /* The OEM stores prescaler/period into the cfg struct *after* the
     * defaults call — the asm order is preserved here for the same
     * reason. counter_mode/clock_div/rep_counter are explicitly re-
     * zeroed to mirror the OEM's literal-zero stores, even though
     * `tim_time_base_config_init` already left them at 0. */
    cfg.period       = period;
    cfg.counter_mode = 0u;
    cfg.prescaler    = prescaler;
    cfg.clock_div    = 0u;
    cfg.rep_counter  = 0u;

    tim_time_base_init((void *)TIM_BASE_TIM2, &cfg);

    const nvic_cfg_t nvic = { .irq = 15u, .priority = 1u, .enable = 1u };
    nvic_configure(&nvic);

    tim_clear_flag((void *)TIM_BASE_TIM2, 1u);   /* SR &= ~UIF */
    tim_dier_bits((void *)TIM_BASE_TIM2, 1u, 1); /* DIER |= UIE */
    tim_enable((void *)TIM_BASE_TIM2, 1);        /* CR1 |= CEN */
}
