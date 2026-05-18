#ifndef SHIFTER_TIMER_H
#define SHIFTER_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#define SYSCLK_HZ    48000000u

void     systick_init(void);
uint32_t systick_millis(void);
void     systick_delay_ms(uint32_t ms);

void     tim3_step_init(uint32_t step_hz);
void     tim3_step_set_rate(uint32_t step_hz);
void     tim3_step_start(void);
void     tim3_step_stop(void);

void     SysTick_Handler(void);
void     TIM3_IRQHandler(void);

/* Called from TIM3_IRQHandler when one step has elapsed.
 * Implemented by the motor driver. */
void     motor_step_tick(void);

/* ---- OEM TIM HAL ------------------------------------------------------ *
 * Shared TIM-init helpers, one per OEM symbol. The OEM uses these for
 * both general-purpose timers (TIM2/3/14) and the advanced timers (TIM1/
 * 15/16) — the helpers branch on the base address. */

/* Mirrors the OEM `TIM_TimeBaseInitTypeDef` struct (32 bytes,
 * field offsets confirmed by `timer_config_init` and the loads in
 * `timer_time_base_init`). The OEM accesses some fields as `ushort` and
 * one (`rep_counter`) as `byte`; we declare matching widths. */
typedef struct {
    uint16_t prescaler;     /* 0x00, written to TIMx->PSC */
    uint16_t _pad0;         /* 0x02 (alignment) */
    uint16_t counter_mode;  /* 0x04, OR'd into TIMx->CR1 (after masking with 0xFF8F) */
    uint16_t _pad1;         /* 0x06 (alignment) */
    uint32_t period;        /* 0x08, written to TIMx->ARR */
    uint32_t clock_div;     /* 0x0C, unused by the OEM helper */
    uint8_t  rep_counter;   /* 0x10, written to TIMx->RCR on advanced timers only */
} tim_time_base_cfg_t;

void tim_time_base_config_init(tim_time_base_cfg_t *cfg);
void tim_time_base_init(void *tim_base, const tim_time_base_cfg_t *cfg);
void tim_clear_flag(void *tim_base, uint16_t flag);
void tim_dier_bits(void *tim_base, uint16_t mask, int enable);
void tim_enable(void *tim_base, int enable);

/* High-level wrapper: configure TIM2 for a periodic update IRQ at
 * `HCLK / ((prescaler+1) * (period+1))` Hz. Called once from `main`
 * during boot with `(HCLK/100000 - 1, 99)` → 1 kHz on a 48 MHz HCLK. */
void tim2_init_periodic(uint16_t prescaler, uint16_t period);

#endif /* SHIFTER_TIMER_H */
