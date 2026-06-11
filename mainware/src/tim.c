#include <stdint.h>

#include "tim.h"
#include "panic.h"   /* Error_Handler */

/* Timer subsystem bring-up. The four board init wrappers (OEM tim1_pwm_init
 * 0x0803C4F4, tim6_init 0x0803C2E0, tim7_init 0x0803C32C, tim10_init
 * 0x0803C37C) set up each handle's TIM_Base_InitTypeDef and drive it through
 * the CubeF4 HAL TIM core; their MSP callbacks (GPIO-AF + RCC clock + NVIC)
 * are reconstructed below. The generic HAL TIM core is still vendor-class and
 * left extern. */

/* ── peripheral bases ─────────────────────────────────────────────────────── */
#define TIM1   ((TIM_TypeDef *)0x40010000u)   /* APB2, advanced — PWM lamps */
#define TIM6   ((TIM_TypeDef *)0x40001000u)   /* APB1, basic */
#define TIM7   ((TIM_TypeDef *)0x40001400u)   /* APB1, basic */
#define TIM8   ((TIM_TypeDef *)0x40010400u)   /* APB2, advanced (MOE check) */
#define TIM10  ((TIM_TypeDef *)0x40014400u)   /* APB2, shares TIM1_UP IRQ */

#define RCC_AHB1ENR  (*(volatile uint32_t *)0x40023830u)
#define RCC_APB1ENR  (*(volatile uint32_t *)0x40023840u)
#define RCC_APB2ENR  (*(volatile uint32_t *)0x40023844u)

#define GPIOE  ((void *)0x40021000u)

/* OEM handles (fixed SRAM addresses; same blocks the TIM vector handlers use). */
#define HTIM1   ((tim_handle_t *)0x20009A84u)
#define HTIM6   ((tim_handle_t *)0x20009A44u)
#define HTIM7   ((tim_handle_t *)0x20009AC4u)
#define HTIM10  ((tim_handle_t *)0x20009A04u)

/* ── generic CubeF4 HAL TIM core (still vendor-class, extern) ──────────────── */
extern int  HAL_TIM_Base_Init(tim_handle_t *htim);                                  /* 0x080277B0 */
extern int  HAL_TIM_PWM_Init(tim_handle_t *htim);                                   /* 0x080277E4 */
extern int  HAL_TIM_ConfigClockSource(tim_handle_t *htim, const void *cfg);         /* 0x08026DC0 */
extern int  HAL_TIM_PWM_ConfigChannel(tim_handle_t *htim, const void *cfg, uint32_t channel); /* 0x08027888 */
extern int  HAL_TIMEx_ConfigBreakDeadTime(tim_handle_t *htim, const void *cfg);     /* 0x08026E48 */
extern void TIM_CCxChannelCmd(TIM_TypeDef *tim, uint32_t channel, uint32_t state);  /* 0x08027964 */
extern void HAL_GPIO_Init(void *GPIOx, const void *init);                           /* 0x080267D0 */
extern void nvic_set_priority(int32_t irq_n, uint32_t preempt, uint32_t sub);       /* 0x08027078 */
extern void nvic_enable_irq(int32_t irq_n);                                         /* 0x080270E0 */

/* Stack config blocks handed to the HAL core (word layout matches what the
 * core reads — only the OEM-populated fields are non-zero). */
typedef struct {
    uint32_t ClockSource, ClockPolarity, ClockPrescaler, ClockFilter;
} tim_clock_config_t;

typedef struct {
    uint32_t OCMode, Pulse, OCPolarity, OCNPolarity, OCFastMode, OCIdleState, OCNIdleState;
} tim_oc_init_t;

typedef struct {
    uint32_t OffStateRunMode, OffStateIDLEMode, LockLevel, DeadTime,
             BreakState, BreakPolarity, AutomaticOutput, BreakFilter;
} tim_breakdeadtime_config_t;

/* GPIO_InitTypeDef view with the Alternate field the TIM1 AF pins need. */
typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} gpio_af_init_t;

/* ── TIM1: three PWM channels (the lamp LEDs on PE9/PE11/PE13) ─────────────── */
void tim1_pwm_init(void)
{
    tim_clock_config_t         sClk   = { 0, 0, 0, 0 };
    tim_oc_init_t              sConfigOC = { 0, 0, 0, 0, 0, 0, 0 };
    tim_breakdeadtime_config_t sBreak = { 0, 0, 0, 0, 0, 0, 0, 0 };

    HTIM1->Instance               = TIM1;
    HTIM1->Init.Prescaler         = 0x960;   /* 2400 */
    HTIM1->Init.CounterMode       = 0;       /* up */
    HTIM1->Init.Period            = 99;
    HTIM1->Init.ClockDivision     = 0;
    HTIM1->Init.RepetitionCounter = 0;
    HTIM1->Init.AutoReloadPreload = 0;
    if (HAL_TIM_PWM_Init(HTIM1) != 0) {
        Error_Handler();
    }

    if (HAL_TIM_ConfigClockSource(HTIM1, &sClk) != 0) {
        Error_Handler();
    }

    sConfigOC.OCMode = 0x60;   /* PWM mode 1 */
    if (HAL_TIM_PWM_ConfigChannel(HTIM1, &sConfigOC, 0) != 0) {   /* CH1 */
        Error_Handler();
    }
    if (HAL_TIM_PWM_ConfigChannel(HTIM1, &sConfigOC, 4) != 0) {   /* CH2 */
        Error_Handler();
    }
    if (HAL_TIM_PWM_ConfigChannel(HTIM1, &sConfigOC, 8) != 0) {   /* CH3 */
        Error_Handler();
    }

    sBreak.BreakPolarity = 0x2000;   /* TIM_BREAKPOLARITY_HIGH */
    if (HAL_TIMEx_ConfigBreakDeadTime(HTIM1, &sBreak) != 0) {
        Error_Handler();
    }

    HAL_TIM_MspPostInit(HTIM1);
}

/* ── TIM6 / TIM7 / TIM10: basic time bases ────────────────────────────────── */
void tim6_init(void)
{
    tim_clock_config_t sClk = { 0, 0, 0, 0 };

    HTIM6->Instance               = TIM6;
    HTIM6->Init.Prescaler         = 0x42;   /* 66 */
    HTIM6->Init.CounterMode       = 0;
    HTIM6->Init.Period            = 0x32;   /* 50 */
    HTIM6->Init.AutoReloadPreload = 0;
    if (HAL_TIM_Base_Init(HTIM6) != 0) {
        Error_Handler();
    }
    if (HAL_TIM_ConfigClockSource(HTIM6, &sClk) != 0) {
        Error_Handler();
    }
}

void tim7_init(void)
{
    tim_clock_config_t sClk = { 0, 0, 0, 0 };

    HTIM7->Instance               = TIM7;
    HTIM7->Init.Prescaler         = 0x4AF;   /* 1199 */
    HTIM7->Init.CounterMode       = 0;
    HTIM7->Init.Period            = 10000;
    HTIM7->Init.AutoReloadPreload = 0;
    if (HAL_TIM_Base_Init(HTIM7) != 0) {
        Error_Handler();
    }
    if (HAL_TIM_ConfigClockSource(HTIM7, &sClk) != 0) {
        Error_Handler();
    }
}

void tim10_init(void)
{
    HTIM10->Instance               = TIM10;
    HTIM10->Init.Prescaler         = 0x5F;     /* 95 */
    HTIM10->Init.CounterMode       = 0;
    HTIM10->Init.Period            = 5000;
    HTIM10->Init.ClockDivision     = 0;
    HTIM10->Init.AutoReloadPreload = 0;
    if (HAL_TIM_Base_Init(HTIM10) != 0) {
        Error_Handler();
    }
}

/* HAL_TIM_PWM_Start core: enable the capture/compare output, the advanced
 * timer's main output (MOE), and the counter (unless it is slaved in trigger
 * mode 6). OEM 0x08027988. */
uint32_t tim_channel_enable_output(tim_handle_t *htim, uint32_t channel)
{
    TIM_TypeDef *tim;

    TIM_CCxChannelCmd(htim->Instance, channel, 1u);
    tim = htim->Instance;
    if (tim == TIM1 || tim == TIM8) {
        tim->BDTR |= 0x8000u;   /* MOE */
    }
    if ((tim->SMCR & 7u) != 6u) {
        tim->CR1 |= 1u;         /* CEN */
    }
    return 0u;
}

/* ── HAL MSP callbacks (board GPIO-AF + RCC clock + NVIC) ──────────────────── */

/* TIM1 PWM clock + update IRQ. OEM 0x0803C3AC. */
void HAL_TIM_PWM_MspInit(tim_handle_t *htim)
{
    if (htim->Instance == TIM1) {
        RCC_APB2ENR |= 0x1u;          /* TIM1EN */
        nvic_set_priority(25, 0, 0);  /* TIM1_UP_TIM10_IRQn */
        nvic_enable_irq(25);
    }
}

/* TIM6 / TIM7 / TIM10 clock + IRQ. OEM 0x0803C3EC. */
void HAL_TIM_Base_MspInit(tim_handle_t *htim)
{
    if (htim->Instance == TIM6) {
        RCC_APB1ENR |= 0x10u;         /* TIM6EN */
        nvic_set_priority(54, 0, 0);  /* TIM6_DAC_IRQn */
        nvic_enable_irq(54);
    } else if (htim->Instance == TIM7) {
        RCC_APB1ENR |= 0x20u;         /* TIM7EN */
        nvic_set_priority(55, 0, 0);  /* TIM7_IRQn */
        nvic_enable_irq(55);
    } else if (htim->Instance == TIM10) {
        RCC_APB2ENR |= 0x20000u;      /* TIM10EN */
        nvic_set_priority(25, 0, 0);  /* TIM1_UP_TIM10_IRQn */
        nvic_enable_irq(25);
    }
}

/* TIM1 PWM output pins: PE9/PE11/PE13 to AF1 (CH1/CH2/CH3). OEM 0x0803C494. */
void HAL_TIM_MspPostInit(tim_handle_t *htim)
{
    gpio_af_init_t gi;

    if (htim->Instance == TIM1) {
        RCC_AHB1ENR |= 0x10u;   /* GPIOEEN */
        gi.Pin       = 0x2A00;  /* PE9 | PE11 | PE13 */
        gi.Mode      = 2;       /* AF push-pull */
        gi.Pull      = 0;
        gi.Speed     = 0;
        gi.Alternate = 1;       /* AF1 = TIM1 */
        HAL_GPIO_Init(GPIOE, &gi);
    }
}

/* TIM1 PWM clock teardown. OEM 0x0803C5D0. */
void HAL_TIM_PWM_MspDeInit(tim_handle_t *htim)
{
    if (htim->Instance == TIM1) {
        RCC_APB2ENR &= ~0x1u;   /* TIM1EN off */
    }
}
