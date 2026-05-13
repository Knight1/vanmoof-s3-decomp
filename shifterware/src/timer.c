/* timer.c — SysTick millis counter and TIM3 step-pulse generator. */

#include "timer.h"
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
