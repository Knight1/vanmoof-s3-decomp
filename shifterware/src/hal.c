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
