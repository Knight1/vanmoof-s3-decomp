#include "batteryware.h"

/* NVIC ISER — interrupt set-enable register array */
static volatile uint32_t * const s_nvic_iser = (volatile uint32_t *)0xE000E100;

/*
 * Enable an NVIC interrupt (Cortex-M0+ ISER register).
 * Only enables if irqn < 0x80 (128 IRQ max on M0+).
 */
void nvic_enable_irq(uint8_t irqn)
{
    if (irqn < 0x80) {
        s_nvic_iser[0] = 1U << (irqn & 0x1F);
    }
}

/*
 * Enable an NVIC interrupt with DSB+ISB barriers.
 * Used when the interrupt must be immediately active (e.g. flash ops).
 */
void nvic_enable_irq_dsb(uint8_t irqn)
{
    if (irqn < 0x80) {
        s_nvic_iser[0] = 1U << (irqn & 0x1F);
        __DSB();
        __ISB();
    }
}

/*
 * Wrappers taking signed char — sign-extends to uint8_t for range check.
 */
void nvic_enable_irq_s(int8_t irqn)
{
    nvic_enable_irq((uint8_t)irqn);
}

void nvic_enable_irq_s_dsb(int8_t irqn)
{
    nvic_enable_irq_dsb((uint8_t)irqn);
}
