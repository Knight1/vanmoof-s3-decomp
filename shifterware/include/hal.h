/* hal.h — small HAL-leaf functions the OEM shares across modules
 * (RCC peripheral-clock toggle + NVIC priority/enable). Each function
 * here corresponds to one OEM symbol; the addresses are in the per-
 * function comment in hal.c. */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>

typedef struct {
    uint8_t irq;            /* 0x00 */
    uint8_t priority;       /* 0x01 */
    uint8_t enable;         /* 0x02 — 1 = enable in ISER, 0 = disable in ICER */
} nvic_cfg_t;

void nvic_configure(const nvic_cfg_t *cfg);

/* CMSIS-style NVIC priority setter. Negative `irq` indexes the
 * Cortex-M0 system handlers (SVCall=-5, PendSV=-2, SysTick=-1) via
 * SCB->SHP at 0xE000ED18; non-negative `irq` indexes the external
 * interrupts via NVIC->IPR at 0xE000E400. Only the top 2 bits of
 * `priority` are honoured (Cortex-M0 has 2 implemented priority
 * bits). */
void nvic_set_priority(int irq, int priority);

void rcc_ahben_bits(uint32_t mask, int enable);
void rcc_apb1en_bits(uint32_t mask, int enable);
void rcc_apb2en_bits(uint32_t mask, int enable);

/* RMW on the peripheral-reset registers (RCC->APB1RSTR @ offset 0x10,
 * RCC->APB2RSTR @ offset 0x0C). `enable=1` sets the bits, `0` clears. */
void rcc_apb1_reset_bits(uint32_t mask, int enable);
void rcc_apb2_reset_bits(uint32_t mask, int enable);

/* Pulse the peripheral reset bit for USART1 or USART2 (decoded from
 * the base-address argument). */
void rcc_reset_usart(void *u);

typedef struct {
    uint32_t sysclk;    /* 0x00 */
    uint32_t hclk;      /* 0x04 */
    uint32_t pclk1;     /* 0x08 */
    uint32_t pclk2;     /* 0x0C */
} rcc_clocks_t;

void rcc_get_clocks_freq(rcc_clocks_t *out);

#endif
