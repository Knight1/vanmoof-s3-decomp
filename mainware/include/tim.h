#ifndef MAINWARE_TIM_H
#define MAINWARE_TIM_H

#include <stdint.h>

/* STM32F413 timer subsystem. TIM1 drives the three PWM lamp channels
 * (CH1/2/3 on PE9/PE11/PE13, AF1); TIM6/TIM7/TIM10 are the basic time-base
 * timers used by the scheduler / sampling. The init wrappers and their HAL MSP
 * callbacks live in tim.c; the generic CubeF4 HAL TIM core (Base_Init,
 * PWM_Init, ConfigChannel, …) is declared extern there. See docs/hardware.md. */

/* TIM register block (STM32F4 standard layout). */
typedef struct {
    volatile uint32_t CR1;    /* 0x00 */
    volatile uint32_t CR2;    /* 0x04 */
    volatile uint32_t SMCR;   /* 0x08 */
    volatile uint32_t DIER;   /* 0x0C */
    volatile uint32_t SR;     /* 0x10 */
    volatile uint32_t EGR;    /* 0x14 */
    volatile uint32_t CCMR1;  /* 0x18 */
    volatile uint32_t CCMR2;  /* 0x1C */
    volatile uint32_t CCER;   /* 0x20 */
    volatile uint32_t CNT;    /* 0x24 */
    volatile uint32_t PSC;    /* 0x28 */
    volatile uint32_t ARR;    /* 0x2C */
    volatile uint32_t RCR;    /* 0x30 */
    volatile uint32_t CCR1;   /* 0x34 */
    volatile uint32_t CCR2;   /* 0x38 */
    volatile uint32_t CCR3;   /* 0x3C */
    volatile uint32_t CCR4;   /* 0x40 */
    volatile uint32_t BDTR;   /* 0x44 */
} TIM_TypeDef;

/* TIM_Base_InitTypeDef (handle +0x04). */
typedef struct {
    uint32_t Prescaler;          /* 0x00 */
    uint32_t CounterMode;        /* 0x04 */
    uint32_t Period;             /* 0x08 */
    uint32_t ClockDivision;      /* 0x0C */
    uint32_t RepetitionCounter;  /* 0x10 */
    uint32_t AutoReloadPreload;  /* 0x14 */
} tim_base_init_t;

/* TIM_HandleTypeDef — only the fields the firmware touches are named; the hdma
 * array pads the gap so Lock(+0x3C)/State(+0x3D) land at the OEM offsets. */
typedef struct {
    TIM_TypeDef    *Instance;   /* 0x00 */
    tim_base_init_t Init;       /* 0x04 */
    uint32_t        Channel;    /* 0x1C */
    void           *hdma[7];    /* 0x20 */
    uint8_t         Lock;       /* 0x3C */
    volatile uint8_t State;     /* 0x3D */
    uint8_t         pad[2];     /* 0x3E */
} tim_handle_t;

void     tim1_pwm_init(void);
void     tim6_init(void);
void     tim7_init(void);
void     tim10_init(void);
uint32_t tim_channel_enable_output(tim_handle_t *htim, uint32_t channel);

void HAL_TIM_PWM_MspInit(tim_handle_t *htim);    /* OEM 0x0803C3AC */
void HAL_TIM_Base_MspInit(tim_handle_t *htim);   /* OEM 0x0803C3EC */
void HAL_TIM_MspPostInit(tim_handle_t *htim);    /* OEM 0x0803C494 */
void HAL_TIM_PWM_MspDeInit(tim_handle_t *htim);  /* OEM 0x0803C5D0 */

/* Weak application hook the TIM6 update ISR calls first; empty in this build
 * (OEM 0x08037AA8). */
void tim6_app_hook(void);

/* De-init the TIM1 PWM (lamp) channels; pre-sleep teardown. OEM name
 * `spi_handle_deinit` (0x0803C614) is a misnomer — it calls HAL_TIM_PWM_DeInit. */
void spi_handle_deinit(void);

/* Stop the TIM6 time base (HAL_TIM_Base_Stop_IT); pre-sleep teardown.
 * OEM peripheral_disable_handle at 0x08037A98. */
void peripheral_disable_handle(void);

#endif
