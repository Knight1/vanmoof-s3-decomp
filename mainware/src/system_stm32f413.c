#include <stdint.h>

#include "system_stm32f413.h"

/* Minimal register definitions for SystemInit. SCB is a Cortex-M core block;
 * RCC is the STM32F4 reset-and-clock controller (RM0430 §6). */
#define SCB_CPACR    (*(volatile uint32_t *)0xE000ED88u)  /* coprocessor access */
#define SCB_VTOR     (*(volatile uint32_t *)0xE000ED08u)  /* vector table offset */
#define RCC_CR       (*(volatile uint32_t *)0x40023800u)  /* clock control */
#define RCC_PLLCFGR  (*(volatile uint32_t *)0x40023804u)  /* PLL config */
#define RCC_CFGR     (*(volatile uint32_t *)0x40023808u)  /* clock config */
#define RCC_CIR      (*(volatile uint32_t *)0x4002380Cu)  /* clock interrupt */

/* Stock CubeF4 SystemInit (OEM 0x08043AA4). The decompile maps 1:1 onto the
 * ST template; the only board-specific value is VECT_TAB_OFFSET = 0 (so VTOR
 * lands on the flash base, not mainware's slot — main() re-points it). */
void SystemInit(void)
{
    /* CP10/CP11 full access — enables the single-precision FPU. */
    SCB_CPACR |= 0x00F00000u;

    RCC_CR |= 0x00000001u;       /* HSION */
    RCC_CFGR = 0x00000000u;
    RCC_CR &= 0xFEF6FFFFu;       /* clear HSEON, CSSON, PLLON */
    RCC_PLLCFGR = 0x24003010u;   /* PLLCFGR reset value */
    RCC_CR &= 0xFFFBFFFFu;       /* clear HSEBYP */
    RCC_CIR = 0x00000000u;       /* disable all RCC interrupts */

    SCB_VTOR = 0x08000000u;      /* flash base | VECT_TAB_OFFSET(0) */
}
