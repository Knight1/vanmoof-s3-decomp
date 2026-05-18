/* hal.c — RCC peripheral-clock and NVIC priority helpers. The OEM
 * has one copy of each, called from uart.c (USART1 init), timer.c
 * (TIM2 init), main.c boot prologue, and several other modules. */

#include "hal.h"
#include "mm32f031.h"

/* OEM @ 0x08004E74 (106 B). */
void nvic_configure(const nvic_cfg_t *cfg)
{
    volatile uint32_t *const nvic = (volatile uint32_t *)0xE000E100u;
    if (cfg->enable != 0u) {
        volatile uint32_t *ip = (volatile uint32_t *)((char *)nvic + 0x300);
        const uint32_t shift = (uint32_t)(cfg->irq & 0x3u) * 8u + 4u;
        const uint32_t idx   = (uint32_t)cfg->irq >> 2;
        uint32_t w = ip[idx];
        w &= ~(0xFFu << shift);
        w |= (((uint32_t)cfg->priority & 0xFu) << shift);
        ip[idx] = w;
        nvic[0] = 1u << (cfg->irq & 0x1Fu);                /* ISER[0] */
    } else {
        volatile uint32_t *icer = (volatile uint32_t *)((char *)nvic + 0x80);
        icer[0] = 1u << (cfg->irq & 0x1Fu);
    }
}

/* OEM @ 0x080030F0 (110 B). CMSIS-style `NVIC_SetPriority`. Two
 * branches selected by the sign of `irq`:
 *   irq < 0  → write into SCB->SHP word at SCB+0x14 + (((irq&0xF)-8)
 *              & ~3), byte position `(irq<<30)>>27`. For SysTick (-1)
 *              that resolves to SCB+0x18 byte 3 = the SHP slot at
 *              SCB+0x1B, per the Cortex-M0 SHP layout.
 *   irq ≥ 0 → write into NVIC->IPR word at NVIC+0x300 + (irq&~3),
 *              byte position `(irq<<30)>>27` (i.e. irq&3 within the
 *              word).
 * The priority byte itself is `((priority<<30)>>24)` — i.e.
 * `(priority & 3) << 6` — Cortex-M0 has 2 implemented priority bits
 * in the top half of each 8-bit field, so values 0..3 map to byte
 * values 0/0x40/0x80/0xC0. */
void nvic_set_priority(int irq, int priority)
{
    const uint32_t shift = ((uint32_t)irq << 30) >> 27;     /* 0/8/16/24 */
    const uint32_t pbyte = ((uint32_t)priority << 30) >> 24;/* (prio&3)<<6 */
    volatile uint32_t *word;
    if (irq < 0) {
        const uintptr_t off = ((uint32_t)(irq & 0xF) - 8u) & ~3u;
        word = (volatile uint32_t *)(SCB_BASE + 0x14u + off);
    } else {
        word = (volatile uint32_t *)(NVIC_BASE + 0x300u + ((uint32_t)irq & ~3u));
    }
    *word = (*word & ~(0xFFu << shift)) | (pbyte << shift);
}

/* OEM @ 0x080051A8 (28 B). */
void rcc_ahben_bits(uint32_t mask, int enable)
{
    if (enable) RCC->AHBENR |= mask;
    else        RCC->AHBENR &= ~mask;
}

/* OEM @ 0x080051E0 (28 B). RCC->APB1ENR sits at base + 0x1C. */
void rcc_apb1en_bits(uint32_t mask, int enable)
{
    volatile uint32_t *const apb1enr = (volatile uint32_t *)((uintptr_t)RCC + 0x1Cu);
    if (enable) *apb1enr |= mask;
    else        *apb1enr &= ~mask;
}

/* OEM @ 0x080051C4 (28 B). */
void rcc_apb2en_bits(uint32_t mask, int enable)
{
    if (enable) RCC->APB2ENR |= mask;
    else        RCC->APB2ENR &= ~mask;
}
