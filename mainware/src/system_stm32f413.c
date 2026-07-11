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

/* ── Cortex-M4 NVIC helpers (CMSIS-equivalent; NVIC base 0xE000E100) ────────── */

/* nvic_enable_irq (OEM 0x080270E0) — NVIC_ISER[irq>>5] = 1<<(irq&31). */
void nvic_enable_irq(int irq_n)
{
    if (irq_n >= 0) {
        *(volatile uint32_t *)(0xE000E100u + ((uint32_t)irq_n >> 5) * 4) =
            1u << (irq_n & 0x1f);
    }
}

/* NVIC_DisableIRQ (OEM 0x080270FC) — NVIC_ICER[irq>>5] = 1<<(irq&31), + DSB/ISB. */
void NVIC_DisableIRQ(int IRQn)
{
    if (IRQn >= 0) {
        *(volatile uint32_t *)(0xE000E100u + (((uint32_t)IRQn >> 5) + 0x20) * 4) =
            1u << (IRQn & 0x1f);
        __asm volatile ("dsb sy" ::: "memory");
        __asm volatile ("isb sy" ::: "memory");
    }
}

/* nvic_clear_pending_irq (OEM 0x0802714C) — NVIC_ICPR[irq>>5] = 1<<(irq&31). */
void nvic_clear_pending_irq(int irq_n)
{
    if (irq_n >= 0) {
        *(volatile uint32_t *)(0xE000E100u + (((uint32_t)irq_n >> 5) + 0x60) * 4) =
            1u << (irq_n & 0x1f);
    }
}

/* nvic_set_priority (OEM 0x08027078) — CMSIS __NVIC_SetPriority: encode preempt +
 * sub priority per SCB_AIRCR.PRIGROUP, then write the 8-bit priority (top 4 bits
 * implemented) into SCB SHP (system exceptions, irq<0) or NVIC IPR (irq>=0). */
void nvic_set_priority(int irq_n, uint32_t preempt_priority, uint32_t sub_priority)
{
    uint32_t prigroup = (*(volatile uint32_t *)0xE000ED0Cu & 0x7ffu) >> 8;   /* AIRCR PRIGROUP */
    uint32_t preempt_bits = 7 - prigroup;
    uint32_t sub_shift;
    uint32_t prio;

    if (preempt_bits > 3) {
        preempt_bits = 4;
    }
    sub_shift = (prigroup + 4 < 7) ? 0 : (prigroup - 3);
    prio = ((preempt_priority & ~(0xFFFFFFFFu << preempt_bits)) << sub_shift) |
           (sub_priority & ~(0xFFFFFFFFu << sub_shift));
    if (irq_n < 0) {
        *(volatile uint8_t *)(0xE000ED14u + (irq_n & 0xf)) = (uint8_t)(prio << 4);
    } else {
        *(volatile uint8_t *)((uint32_t)irq_n + 0xE000E400u) = (uint8_t)(prio << 4);
    }
}

/* enter_low_power_wait (OEM 0x08022DC4) — the core STOP-mode sleep primitive: set
 * PWR_CR.LPDS|PDDS (bits 0-1) to the requested regulator/standby mode, assert
 * SCB->SCR.SLEEPDEEP, then park the CPU with WFI (use_wfi == 1) or a double WFE.
 * On wake, clear SLEEPDEEP so ordinary WFI sleeps stay shallow. Driven by
 * enter_stop_mode after the pre-sleep peripheral de-init cascade. */
void enter_low_power_wait(uint32_t pwr_cr_mode, int use_wfi)
{
    volatile uint32_t *pwr_cr  = (volatile uint32_t *)0x40007000u;   /* PWR_CR   */
    volatile uint32_t *scb_scr = (volatile uint32_t *)0xE000ED10u;   /* SCB->SCR */

    *pwr_cr  = (*pwr_cr & 0xfffffffcu) | pwr_cr_mode;
    *scb_scr |= 4u;                     /* SLEEPDEEP */
    if (use_wfi == 1) {
        __asm volatile ("wfi");
    } else {
        __asm volatile ("wfe");
        __asm volatile ("wfe");
    }
    *scb_scr &= ~4u;                    /* clear SLEEPDEEP */
}
