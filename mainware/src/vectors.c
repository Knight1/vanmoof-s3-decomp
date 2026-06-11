#include <stdint.h>

#include "gpio.h"     /* HAL_GPIO_EXTI_IRQHandler(uint16_t) */
#include "vectors.h"

/* NVIC peripheral interrupt vectors (the 16..127 external-IRQ slots of the
 * table at flash 0x08020200). Each is a thin trampoline the CPU enters on the
 * corresponding NVIC line; it forwards to the real handler — a CubeF4 HAL
 * IRQ servicer, a serial byte-pump ISR, or an EXTI-line demux — sometimes after
 * an application pre-hook. The OEM addresses (0x0803CA20..0x0803CB64) and the
 * peripheral assignment of every slot were read straight from the vector table:
 * slot index == IRQ number (verified against USART1=37, UART4=52, USART6=71,
 * UART7=82, UART8=83 by direct slot reads). That vector mapping is also what
 * pins down each serial peripheral's identity — UART7 carries the ES3 console,
 * UART8 the BLE-debug link, UART4 the 9600-baud BMS bus, UART5 the 115200-baud
 * BLE data link.
 *
 * These bodies are reconstructed but deliberately NOT wired into startup.S yet:
 * several leaf handlers (the HAL servicers and the EXTI/TIM application hooks)
 * are still un-sourced, so rooting the table here would pull undefined symbols.
 * They compile and gc away until the handler closure is complete (same staging
 * as src/main.c's super-loop). */

/* ── Leaf handlers (OEM addresses; defined in their own modules / still un-sourced) ── */
extern void usart1_irq_handler(void);   /* 0x08035F98  console 2nd port (console.c) */
extern void usart2_irq_handler(void);   /* GSM modem (modem.c) */
extern void usart3_irq_handler(void);   /* 0x080362D0  eShifter Modbus (shifter.c) */
extern void uart4_irq_handler(void);    /* 0x08036424  BMS Modbus, 9600 (bus.c) */
extern void uart5_irq_handler(void);    /* 0x08036560  BLE data link, 115200 (uart.c) */
extern void usart6_irq_handler(void);   /* inter-module SSPM bus (ssp.c) */
extern void uart7_irq_handler(void);    /* 0x080368D4  ES3 console primary (console.c) */
extern void uart8_irq_handler(void);    /* 0x08036AA8  BLE-debug link (uart.c) */

/* CubeF4 HAL IRQ servicers, shared across instances via the passed handle. */
extern void rtc_wakeup_irq_handler(void *hrtc); /* 0x08027020  RTC wake-up timer */
extern void HAL_DMA_IRQHandler(void *hdma);     /* 0x08022BDC */
extern void HAL_TIM_IRQHandler(void *htim);     /* 0x0802756C */
extern void HAL_I2C_EV_IRQHandler(void *hi2c);  /* 0x08025E04 */
extern void HAL_I2C_ER_IRQHandler(void *hi2c);  /* 0x08025F9E */

/* Application pre-hooks run ahead of the HAL/EXTI servicer (not yet sourced). */
extern void exti4_app_hook(void);    /* 0x08043CEC  before EXTI line 4 demux */
extern void exti9_5_app_hook(void);  /* 0x08038FF4  before EXTI lines 5/8 demux */
extern void tim6_app_hook(void);     /* 0x08037AA8  before TIM6 update servicer */
extern void tim7_app_hook(void);     /* 0x08039138  before TIM7 update servicer */

/* HAL handles the trampolines forward to (SRAM addresses from the OEM literal pool). */
#define HRTC_WAKEUP   ((void *)0x200099E4u)
#define HDMA1_STREAM1 ((void *)0x20009B58u)
#define HTIM1_UPDATE  ((void *)0x20009A84u)
#define HTIM10        ((void *)0x20009A04u)
#define HI2C1         ((void *)0x20009BB8u)
#define HTIM6         ((void *)0x20009A44u)
#define HTIM7         ((void *)0x20009AC4u)
#define HDMA2_STREAM0 ((void *)0x20009784u)
#define HI2C3         ((void *)0x20009B04u)

/* EXTI line masks (the OEM passes the raw pin bit). */
#define EXTI_PIN0  0x0001u
#define EXTI_PIN1  0x0002u
#define EXTI_PIN2  0x0004u
#define EXTI_PIN3  0x0008u
#define EXTI_PIN4  0x0010u
#define EXTI_PIN5  0x0020u
#define EXTI_PIN8  0x0100u
#define EXTI_PIN10 0x0400u

void RTC_WKUP_IRQHandler(void)        /* IRQ3,  OEM 0x0803CA20 */
{
    rtc_wakeup_irq_handler(HRTC_WAKEUP);
}

void EXTI0_IRQHandler(void)           /* IRQ6,  OEM 0x0803CA30 */
{
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN0);
}

void EXTI1_IRQHandler(void)           /* IRQ7,  OEM 0x0803CA3A */
{
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN1);
}

void EXTI2_IRQHandler(void)           /* IRQ8,  OEM 0x0803CA44 */
{
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN2);
}

void EXTI3_IRQHandler(void)           /* IRQ9,  OEM 0x0803CA4E */
{
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN3);
}

void EXTI4_IRQHandler(void)           /* IRQ10, OEM 0x0803CA58 */
{
    exti4_app_hook();
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN4);
}

void DMA1_Stream1_IRQHandler(void)    /* IRQ12, OEM 0x0803CA68 */
{
    HAL_DMA_IRQHandler(HDMA1_STREAM1);
}

void EXTI9_5_IRQHandler(void)         /* IRQ23, OEM 0x0803CA78 */
{
    exti9_5_app_hook();
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN5);
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN8);
}

void TIM1_UP_TIM10_IRQHandler(void)   /* IRQ25, OEM 0x0803CA90 */
{
    HAL_TIM_IRQHandler(HTIM1_UPDATE);
    HAL_TIM_IRQHandler(HTIM10);
}

void I2C1_EV_IRQHandler(void)         /* IRQ31, OEM 0x0803CAA8 */
{
    HAL_I2C_EV_IRQHandler(HI2C1);
}

void I2C1_ER_IRQHandler(void)         /* IRQ32, OEM 0x0803CAB8 */
{
    HAL_I2C_ER_IRQHandler(HI2C1);
}

void USART1_IRQHandler(void)          /* IRQ37, OEM 0x0803CAC8 */
{
    usart1_irq_handler();
}

void USART2_IRQHandler(void)          /* IRQ38, OEM 0x0803CAD0 */
{
    usart2_irq_handler();
}

void USART3_IRQHandler(void)          /* IRQ39, OEM 0x0803CAD8 */
{
    usart3_irq_handler();
}

void EXTI15_10_IRQHandler(void)       /* IRQ40, OEM 0x0803CAE0 */
{
    HAL_GPIO_EXTI_IRQHandler(EXTI_PIN10);
}

void UART4_IRQHandler(void)           /* IRQ52, OEM 0x0803CAEC */
{
    uart4_irq_handler();
}

void UART5_IRQHandler(void)           /* IRQ53, OEM 0x0803CAF4 */
{
    uart5_irq_handler();
}

void TIM6_DAC_IRQHandler(void)        /* IRQ54, OEM 0x0803CAFC */
{
    tim6_app_hook();
    HAL_TIM_IRQHandler(HTIM6);
}

void TIM7_IRQHandler(void)            /* IRQ55, OEM 0x0803CB10 */
{
    tim7_app_hook();
    HAL_TIM_IRQHandler(HTIM7);
}

void DMA2_Stream0_IRQHandler(void)    /* IRQ56, OEM 0x0803CB24 */
{
    HAL_DMA_IRQHandler(HDMA2_STREAM0);
}

void USART6_IRQHandler(void)          /* IRQ71, OEM 0x0803CB34 */
{
    usart6_irq_handler();
}

void I2C3_EV_IRQHandler(void)         /* IRQ72, OEM 0x0803CB3C */
{
    HAL_I2C_EV_IRQHandler(HI2C3);
}

void I2C3_ER_IRQHandler(void)         /* IRQ73, OEM 0x0803CB4C */
{
    HAL_I2C_ER_IRQHandler(HI2C3);
}

void UART7_IRQHandler(void)           /* IRQ82, OEM 0x0803CB5C — ES3 console primary */
{
    uart7_irq_handler();
}

void UART8_IRQHandler(void)           /* IRQ83, OEM 0x0803CB64 — BLE-debug link */
{
    uart8_irq_handler();
}
