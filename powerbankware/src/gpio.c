#include "powerbankware.h"

/*
 * GPIO primitives for the STM32F091 powerbankware.
 *
 * Faithful translations of the OEM GPIO leaf cluster:
 *   gpio_bit_read   = FUN_0801a81c   (IDR test)
 *   gpio_bit_write  = FUN_0801a856   (BSRR/BRR)
 *   gpio_pin_config = FUN_0801a524   (HAL_GPIO_Init, 730 B)
 *
 * GPIO ports sit on the AHB at 0x48000000 with a 0x400 stride
 * (A=...000, B=...400, C=...800, D=...c00, E=...1000). The EXTI/SYSCFG
 * wiring resolves to RCC 0x40021000, SYSCFG 0x40010000, EXTI 0x40010400
 * (literal-pool values from the OEM image).
 */

/* GPIO port bases on the AHB (0x400 stride). */
#define GPIOA_BASE  0x48000000u
#define GPIOB_BASE  0x48000400u
#define GPIOC_BASE  0x48000800u
#define GPIOD_BASE  0x48000c00u
#define GPIOE_BASE  0x48001000u

/* GPIO input read via IDR (+0x10). */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit)
{
    return (*(volatile uint32_t *)(gpio_base + 0x10) & (uint32_t)pin_bit) != 0;
}

/*
 * GPIO atomic bit write. value==0 clears the pin via BRR (+0x28); any
 * non-zero value sets it via BSRR (+0x18). Both registers are write-only
 * and atomic, so no read-modify-write is needed.
 */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value)
{
    if (value == 0) {
        *(volatile uint32_t *)(gpio_base + 0x28) = (uint32_t)pin_bit;   /* BRR  */
    } else {
        *(volatile uint32_t *)(gpio_base + 0x18) = (uint32_t)pin_bit;   /* BSRR */
    }
}

/*
 * gpio_pin_config — STM32F0 HAL_GPIO_Init.
 *
 * Walks the bits set in cfg->pin_mask; for each selected pin index i writes:
 *   MODER   (+0x00, always)
 *   OSPEEDR (+0x08) + OTYPER (+0x04)  when mode in {OUTPUT, AF} (with/without OD)
 *   PUPDR   (+0x0C, always)
 *   AFRL/AFRH (+0x20/+0x24)           when mode is AF (with/without OD)
 * and, when the EXTI-enable bit (28) is set, programs SYSCFG_EXTICRx + the
 * EXTI IMR/EMR/RTSR/FTSR lines.
 *
 * Note the F0-specific RCC->APB2ENR offset 0x18 (the L0 sibling uses 0x34).
 */
void gpio_pin_config(uint32_t *gpio_base, gpio_pin_cfg_t *cfg)
{
    volatile uint32_t * const RCC_APB2ENR = (volatile uint32_t *)(0x40021000 + 0x18);
    volatile uint32_t * const SYSCFG      = (volatile uint32_t *)0x40010000;
    volatile uint32_t * const EXTI        = (volatile uint32_t *)0x40010400;

    volatile uint32_t *gp = (volatile uint32_t *)gpio_base;
    const uint32_t mode = cfg->mode;

    for (uint32_t i = 0; (cfg->pin_mask >> i) != 0; i++) {
        const uint32_t pin_mask = cfg->pin_mask & (1U << i);
        if (pin_mask == 0) {
            continue;
        }

        const uint32_t shift2    = i * 2;
        const uint32_t afr_idx   = (i >> 3) + 8;
        const uint32_t afr_shift = (i & 7) * 4;

        /* AFRL/AFRH — alternate-function modes only. */
        if (mode == GPIO_MODE_AF || mode == (GPIO_MODE_AF | GPIO_OTYPE_OD)) {
            gp[afr_idx] = (cfg->af << afr_shift) | (gp[afr_idx] & ~(0xFU << afr_shift));
        }

        /* MODER — always. */
        gp[0] = ((mode & 3U) << shift2) | (gp[0] & ~(3U << shift2));

        /* OSPEEDR + OTYPER — output/AF modes only. */
        if (mode == GPIO_MODE_OUTPUT || mode == GPIO_MODE_AF ||
            mode == (GPIO_MODE_OUTPUT | GPIO_OTYPE_OD) ||
            mode == (GPIO_MODE_AF | GPIO_OTYPE_OD)) {
            gp[2] = (cfg->speed << shift2) | (gp[2] & ~(3U << shift2));
            gp[1] = (((mode >> 4) & 1U) << i) | (gp[1] & ~(1U << i));
        }

        /* PUPDR — always. */
        gp[3] = (cfg->pupd << shift2) | (gp[3] & ~(3U << shift2));

        /* EXTI block — gated on mode bit 28. */
        if ((mode & GPIO_EXTI_ENABLE) != 0) {
            *RCC_APB2ENR |= 1U;                    /* SYSCFG clock enable */

            int port;
            if (gpio_base == (uint32_t *)GPIOA_BASE)       port = 0;  /* A */
            else if (gpio_base == (uint32_t *)GPIOB_BASE)  port = 1;  /* B */
            else if (gpio_base == (uint32_t *)GPIOC_BASE)  port = 2;  /* C */
            else if (gpio_base == (uint32_t *)GPIOD_BASE)  port = 3;  /* D */
            else if (gpio_base == (uint32_t *)GPIOE_BASE)  port = 4;  /* E */
            else                                           port = 5;  /* F */

            volatile uint32_t *exticr = &SYSCFG[(i >> 2) + 2];        /* EXTICR1 @ +0x08 */
            *exticr = ((uint32_t)port << ((i & 3) * 4)) |
                      (*exticr & ~(0xFU << ((i & 3) * 4)));

            uint32_t v;
            v = EXTI[0] & ~pin_mask;                                  /* IMR  */
            if ((mode & GPIO_EXTI_IMR)  != 0) v |= pin_mask;
            EXTI[0] = v;
            v = EXTI[1] & ~pin_mask;                                  /* EMR  */
            if ((mode & GPIO_EXTI_EMR)  != 0) v |= pin_mask;
            EXTI[1] = v;
            v = EXTI[2] & ~pin_mask;                                  /* RTSR */
            if ((mode & GPIO_EXTI_RTSR) != 0) v |= pin_mask;
            EXTI[2] = v;
            v = EXTI[3] & ~pin_mask;                                  /* FTSR */
            if ((mode & GPIO_EXTI_FTSR) != 0) v |= pin_mask;
            EXTI[3] = v;
        }
    }
}
