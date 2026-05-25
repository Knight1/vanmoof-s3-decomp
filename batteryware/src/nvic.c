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

/*
 * NVIC reconfigure — enable/disable peripheral interrupt mask.
 *
 * Takes a flash/DMA context and a configuration word.
 * If param[1] (mask value) == 0xFFFFBFFF (disable key):
 *   - clears bits in ctx+0x28 for *param & 0x7FFFF
 *   - clears RCC bits 0x800000 and 0x400000
 * Otherwise:
 *   - ORs bits in ctx+0x28 with *param & 0x7FFFF
 *   - optionally sets RCC bits 0x800000 (+10µs delay) and 0x400000
 * Returns 0 on success, 2 if BUSY, 1 on error.
 */
uint32_t nvic_reconfigure(int *ctx, uint32_t *param)
{
    if (((uint8_t)ctx[0x14]) == 1) {
        return 2;
    }

    *(volatile uint8_t *)(ctx + 0x14) = 1;
    volatile uint32_t *reg = (volatile uint32_t *)*ctx;

    if ((reg[2] & 4) == 0) {
        volatile uint32_t * const s_rcc = (volatile uint32_t *)0x200047FC;

        if (param[1] == 0xFFFFBFFF) {
            reg[0xA] = reg[0xA] & ~(*param & 0x7FFFF);
            if ((*param & 0x40000) != 0) {
                *s_rcc &= 0xFF7FFFFF;
            }
            if ((*param & 0x20000) != 0) {
                *s_rcc &= 0xFFBFFFFF;
            }
        } else {
            reg[0xA] = (*param & 0x7FFFF) | reg[0xA];
            if ((*param & 0x40000) != 0) {
                *s_rcc |= 0x800000;
                delay_us(10);
            }
            if ((*param & 0x20000) != 0) {
                *s_rcc |= 0x400000;
            }
        }

        *(volatile uint8_t *)(ctx + 0x14) = 0;
        return 0;
    }

    ctx[0x15] |= 0x20;
    *(volatile uint8_t *)(ctx + 0x14) = 0;
    return 1;
}
