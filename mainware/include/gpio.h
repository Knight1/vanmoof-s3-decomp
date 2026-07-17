#ifndef MAINWARE_GPIO_H
#define MAINWARE_GPIO_H

#include <stdint.h>

/* Board GPIO bring-up for the STM32F413 main controller (OEM gpio_init,
 * 0x080314E8): enables the GPIO port clocks, drives the initial output levels
 * (power/enable rails, status LEDs, amp lines), and configures every pin via
 * the CubeF4 HAL_GPIO_Init. This is the mainware pin map — see hardware.md. */
void gpio_init(void);

/* ---------------------------------------------------------------------------
 * CubeF4 GPIO HAL register blocks (stm32f4xx_hal_gpio.c). Used by the
 * de-init / EXTI-IRQ path below; bring-up (gpio_init) goes through the opaque
 * void* externs in gpio.c.
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t MODER;     /* 0x00  mode */
    volatile uint32_t OTYPER;    /* 0x04  output type */
    volatile uint32_t OSPEEDR;   /* 0x08  output speed */
    volatile uint32_t PUPDR;     /* 0x0C  pull up/down */
    volatile uint32_t IDR;       /* 0x10  input data */
    volatile uint32_t ODR;       /* 0x14  output data */
    volatile uint32_t BSRR;      /* 0x18  bit set/reset */
    volatile uint32_t LCKR;      /* 0x1C  config lock */
    volatile uint32_t AFR[2];    /* 0x20  alternate function low/high */
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t IMR;       /* 0x00  interrupt mask */
    volatile uint32_t EMR;       /* 0x04  event mask */
    volatile uint32_t RTSR;      /* 0x08  rising trigger */
    volatile uint32_t FTSR;      /* 0x0C  falling trigger */
    volatile uint32_t SWIER;     /* 0x10  software interrupt */
    volatile uint32_t PR;        /* 0x14  pending */
} EXTI_TypeDef;

typedef struct {
    volatile uint32_t MEMRMP;    /* 0x00 */
    volatile uint32_t PMC;       /* 0x04 */
    volatile uint32_t EXTICR[4]; /* 0x08  external interrupt config 1..4 */
} SYSCFG_TypeDef;

#define EXTI    ((EXTI_TypeDef *)0x40013C00u)
#define SYSCFG  ((SYSCFG_TypeDef *)0x40013800u)

/* Reset a set of pins back to their default (analog input) state and tear down
 * any EXTI line they own (OEM HAL_GPIO_DeInit, 0x08026990). */
void HAL_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint32_t GPIO_Pin);

/* EXTI line ISR demux: clears the pending bit and invokes the user callback
 * (OEM HAL_GPIO_EXTI_IRQHandler, 0x08026AD4). */
void HAL_GPIO_EXTI_IRQHandler(uint16_t GPIO_Pin);

/* Application override of the weak HAL EXTI callback, fired by
 * HAL_GPIO_EXTI_IRQHandler (OEM 0x08038964). Latches which EXTI line woke the
 * bike as a wake-source code (EXTI0..5 -> 2..7, EXTI8 -> 9, EXTI10 -> 8) through
 * g_sleep_ctx+0x24; sibling of rtc_wakeup_event_cb (writes 1 for an RTC wake). */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin);

/* PC0 / PC1 sense lines: 1 if the pin reads LOW, else 0 (OEM 0x08040350 /
 * 0x08040368). The two button-state bytes of the BLE 0x5568 read. */
int gpio_pc0_is_low(void);
int gpio_pc1_is_low(void);

#endif
