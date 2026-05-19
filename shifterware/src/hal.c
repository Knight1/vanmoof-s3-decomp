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

/* OEM @ 0x08005218 (28 B). RMW on RCC->APB1RSTR (offset 0x10). */
void rcc_apb1_reset_bits(uint32_t mask, int enable)
{
    if (enable) RCC->APB1RSTR |= mask;
    else        RCC->APB1RSTR &= ~mask;
}

/* OEM @ 0x080051FC (28 B). RMW on RCC->APB2RSTR (offset 0x0C). */
void rcc_apb2_reset_bits(uint32_t mask, int enable)
{
    if (enable) RCC->APB2RSTR |= mask;
    else        RCC->APB2RSTR &= ~mask;
}

/* OEM @ 0x08005B9C (54 B). Pulse the matching peripheral-reset bit
 * for the USART instance at `u`. The OEM stdlib has a separate set/
 * clear call pair rather than a single "pulse with cycle" — match
 * that exactly. */
void rcc_reset_usart(void *u)
{
    if (u == (void *)0x40004400u) {             /* USART2 (APB1) */
        rcc_apb1_reset_bits(1u << 17, 1);       /* APB1RSTR.USART2RST */
        rcc_apb1_reset_bits(1u << 17, 0);
    }
    if (u == (void *)0x40013800u) {             /* USART1 (APB2) */
        rcc_apb2_reset_bits(1u << 14, 1);       /* APB2RSTR.USART1RST */
        rcc_apb2_reset_bits(1u << 14, 0);
    }
}

/* OEM @ 0x08005108 (160 B). Standard MM32 `RCC_GetClocksFreq`.
 *
 * Decode the active SYSCLK source from RCC->CFGR.SWS, then compute
 * HCLK / PCLK1 / PCLK2 via the AHB/APB prescaler shift table. The
 * literal SYSCLK values were resolved from the OEM literal pool at
 * 0x080052A8..0x080052D0 and **confirmed by the MM32F031 user manual
 * § 5.2 clock-tree diagram** (`reference/mm32f031/UM_MM32F031xx_q_EN.pdf`):
 *   SWS == 0  (HSI)  → HSI/6  =  8 MHz (or 12 MHz when RCC_CR.HSI_72M_EN = 1)
 *   SWS == 1  (HSE)  → 8 MHz  (OEM-shipped HSE crystal frequency)
 *   SWS == 2  (PLL)  → direct HSI = 48 MHz (or 72 MHz when HSI_72M_EN = 1)
 *   SWS == 3  (LSI)  → 40 kHz
 *
 * Note: the "PLL" position is a misnomer — MM32F031's HSI path has
 * no actual multiplier. The hardware feeds the un-divided HSI into
 * the SW=10 slot; the `PLLON` / `PLLMUL` / `PLLSRC` bits in the
 * register layout are STM32F0-inheritance artefacts that the OEM
 * doesn't program. See the `set_sysclock_to_48m` decomp in
 * `shifterboot/src/clock.c` for the actual 4-step bring-up.
 *
 * The OEM stores APBAHBPrescTable in RAM (.data) at 0x2000014C; we
 * put it in flash (.rodata) which is behaviour-equivalent. */
static const uint8_t APBAHBPrescTable[16] = {
    0, 0, 0, 0, 1, 2, 3, 4, 1, 2, 3, 4, 6, 7, 8, 9
};

void rcc_get_clocks_freq(rcc_clocks_t *out)
{
    const uint32_t cfgr = RCC->CFGR;
    const uint32_t cr   = RCC->CR;
    const uint32_t sws  = cfgr & 0x0Cu;

    if (sws == 0x04u) {
        out->sysclk = 8000000u;
    } else if (sws == 0x08u) {
        out->sysclk = ((cr & 0x100000u) == 0u) ? 48000000u : 72000000u;
    } else if (sws == 0x0Cu) {
        out->sysclk = 40000u;
    } else {
        out->sysclk = ((cr & 0x100000u) == 0u) ? 8000000u : 12000000u;
    }
    out->hclk  = out->sysclk >> APBAHBPrescTable[(cfgr & 0x00F0u) >> 4];
    out->pclk1 = out->hclk   >> APBAHBPrescTable[(cfgr & 0x0700u) >> 8];
    out->pclk2 = out->hclk   >> APBAHBPrescTable[(cfgr & 0x3800u) >> 11];
}
