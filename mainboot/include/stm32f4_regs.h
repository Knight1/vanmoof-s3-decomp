#ifndef STM32F4_REGS_H
#define STM32F4_REGS_H

#include <stdint.h>

/*
 * Hand-curated subset of the STM32F4 peripheral map for the muco-boot
 * decomp. Mirror of ST's CMSIS-Device layout (RM0090 §2.3 for F405-F417,
 * RM0386 for F469/F479) — we restate only the addresses and bit
 * positions that the OEM image touches, to keep the build self-contained.
 *
 * Each register is exposed as a `volatile uint32_t *` macro pointing
 * directly at the MMIO word, in the style of the OEM code's literal
 * pool — no struct wrappers, no CMSIS includes.
 */

/* ---- Reset and Clock Control (RCC) ---- */
#define RCC_BASE          0x40023800UL

#define RCC_CR            (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_PLLCFGR       (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_CFGR          (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define RCC_CIR           (*(volatile uint32_t *)(RCC_BASE + 0x0C))
#define RCC_AHB1RSTR      (*(volatile uint32_t *)(RCC_BASE + 0x10))
#define RCC_AHB2RSTR      (*(volatile uint32_t *)(RCC_BASE + 0x14))
#define RCC_AHB3RSTR      (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define RCC_APB1RSTR      (*(volatile uint32_t *)(RCC_BASE + 0x20))
#define RCC_APB2RSTR      (*(volatile uint32_t *)(RCC_BASE + 0x24))

#endif /* STM32F4_REGS_H */
