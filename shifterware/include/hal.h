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

void rcc_ahben_bits(uint32_t mask, int enable);
void rcc_apb1en_bits(uint32_t mask, int enable);
void rcc_apb2en_bits(uint32_t mask, int enable);

#endif
