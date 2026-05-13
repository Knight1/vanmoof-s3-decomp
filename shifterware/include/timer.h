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

#endif /* SHIFTER_TIMER_H */
