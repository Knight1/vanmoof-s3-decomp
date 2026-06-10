#include <stdint.h>

#include "gpio.h"

/* Board GPIO bring-up (OEM gpio_init, 0x080314E8). The RCC clock-enable bits and
 * the per-port pin masks/levels are reproduced verbatim from the image; the
 * per-pin config goes through the CubeF4 HAL_GPIO_Init.
 *
 * GPIO port bases (AHB1): A 0x40020000, B 0x40020400, C 0x40020800,
 * D 0x40020C00, E 0x40021000. RCC_AHB1ENR at 0x40023830.
 *
 * The init record matches the OEM stack frame: four 32-bit slots. The third
 * slot carries the alternate-function index for the GPIOC AF pins (0/1/2); the
 * HAL reads it from the same offset, so behaviour is preserved regardless of
 * whether it is the Pull or Alternate field in the HAL's own struct view. */
#define RCC_AHB1ENR  (*(volatile uint32_t *)0x40023830u)

#define GPIOA_BASE   ((void *)0x40020000u)
#define GPIOB_BASE   ((void *)0x40020400u)
#define GPIOC_BASE   ((void *)0x40020800u)
#define GPIOD_BASE   ((void *)0x40020C00u)
#define GPIOE_BASE   ((void *)0x40021000u)

#define GPIO_MODE_AF_C  0x10110000u   /* AF push-pull, family C (DAT_080316CC) */
#define GPIO_MODE_AF_E  0x10310000u   /* AF push-pull, family E (DAT_080316C0) */

typedef struct {
    uint32_t Pin;     /* +0x00 */
    uint32_t Mode;    /* +0x04 */
    uint32_t Field2;  /* +0x08  Pull, or AF index for the AF pins */
    uint32_t Speed;   /* +0x0C */
} gpio_init_record_t;

extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state); /* 0x08026AC6 */
extern void HAL_GPIO_Init(void *GPIOx, gpio_init_record_t *init);         /* 0x080267D0 */
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);             /* 0x08026AB8 */

/* GPIO_GET_INDEX: bank pointer -> SYSCFG EXTICR nibble value (A=0 .. H=7).
 * The OEM open-codes this as the same chain of base-address compares. */
static uint32_t GPIO_GetIndex(GPIO_TypeDef *GPIOx)
{
    if (GPIOx == (GPIO_TypeDef *)0x40020000u) return 0u;  /* GPIOA */
    if (GPIOx == (GPIO_TypeDef *)0x40020400u) return 1u;  /* GPIOB */
    if (GPIOx == (GPIO_TypeDef *)0x40020800u) return 2u;  /* GPIOC */
    if (GPIOx == (GPIO_TypeDef *)0x40020C00u) return 3u;  /* GPIOD */
    if (GPIOx == (GPIO_TypeDef *)0x40021000u) return 4u;  /* GPIOE */
    if (GPIOx == (GPIO_TypeDef *)0x40021400u) return 5u;  /* GPIOF */
    if (GPIOx == (GPIO_TypeDef *)0x40021800u) return 6u;  /* GPIOG */
    return 7u;                                            /* GPIOH */
}

/* OEM HAL_GPIO_DeInit, 0x08026990. Walks pins 0..15; for each selected pin,
 * tears down its EXTI line (only if SYSCFG still maps that line to this bank)
 * then returns the pad to the reset config (analog input, no pull, no AF). */
void HAL_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint32_t GPIO_Pin)
{
    uint32_t position;

    for (position = 0; position < 16u; position++) {
        uint32_t iocurrent = (1u << position) & GPIO_Pin;
        if (iocurrent == 0u) {
            continue;
        }

        /* EXTI mode config — only clear if this bank still owns the line. */
        uint32_t shift = (position & 3u) << 2;
        uint32_t tmp = SYSCFG->EXTICR[position >> 2] & (0xFu << shift);
        if (tmp == (GPIO_GetIndex(GPIOx) << shift)) {
            EXTI->IMR  &= ~iocurrent;
            EXTI->EMR  &= ~iocurrent;
            EXTI->RTSR &= ~iocurrent;
            EXTI->FTSR &= ~iocurrent;
            SYSCFG->EXTICR[position >> 2] &= ~(0xFu << shift);
        }

        /* GPIO mode config — back to reset state. */
        GPIOx->MODER   &= ~(3u << (position * 2u));
        GPIOx->AFR[position >> 3] &= ~(0xFu << ((position & 7u) << 2));
        GPIOx->PUPDR   &= ~(3u << (position * 2u));
        GPIOx->OTYPER  &= ~(1u << position);
        GPIOx->OSPEEDR &= ~(3u << (position * 2u));
    }
}

/* OEM HAL_GPIO_EXTI_IRQHandler, 0x08026AD4. */
void HAL_GPIO_EXTI_IRQHandler(uint16_t GPIO_Pin)
{
    if ((EXTI->PR & GPIO_Pin) != 0u) {
        EXTI->PR = GPIO_Pin;          /* rc_w1: write 1 to clear pending */
        HAL_GPIO_EXTI_Callback(GPIO_Pin);
    }
}

void gpio_init(void)
{
    gpio_init_record_t gi = { 0, 0, 0, 0 };

    /* Enable GPIO port clocks (AHB1ENR) — exact OR order: E, H, C, A, B, D. */
    RCC_AHB1ENR |= 0x10u;   /* GPIOEEN */
    RCC_AHB1ENR |= 0x80u;   /* GPIOHEN */
    RCC_AHB1ENR |= 0x04u;   /* GPIOCEN */
    RCC_AHB1ENR |= 0x01u;   /* GPIOAEN */
    RCC_AHB1ENR |= 0x02u;   /* GPIOBEN */
    RCC_AHB1ENR |= 0x08u;   /* GPIODEN */

    /* Initial output levels. */
    HAL_GPIO_WritePin(GPIOE_BASE, 0x102C, 0);
    HAL_GPIO_WritePin(GPIOE_BASE, 0x40,   1);
    HAL_GPIO_WritePin(GPIOB_BASE, 0xC32F, 0);
    HAL_GPIO_WritePin(GPIOB_BASE, 0x400,  1);
    HAL_GPIO_WritePin(GPIOD_BASE, 0xBCF0, 0);
    HAL_GPIO_WritePin(GPIOA_BASE, 0x9000, 0);
    HAL_GPIO_WritePin(GPIOD_BASE, 0x1,    1);

    gi.Pin = 0x106C; gi.Mode = 1; gi.Field2 = 0; gi.Speed = 0;   /* GPIOE outputs */
    HAL_GPIO_Init(GPIOE_BASE, &gi);

    gi.Pin = 0x410; gi.Mode = 0; gi.Field2 = 0;                  /* GPIOE inputs */
    HAL_GPIO_Init(GPIOE_BASE, &gi);

    gi.Pin = 0x2F; gi.Mode = GPIO_MODE_AF_C; gi.Field2 = 0;      /* GPIOC AF idx 0 */
    HAL_GPIO_Init(GPIOC_BASE, &gi);

    gi.Pin = 0x10; gi.Mode = GPIO_MODE_AF_E; gi.Field2 = 0;      /* GPIOC AF (family E) */
    HAL_GPIO_Init(GPIOC_BASE, &gi);

    gi.Pin = 0xC72F; gi.Mode = 1; gi.Field2 = 0; gi.Speed = 0;   /* GPIOB outputs */
    HAL_GPIO_Init(GPIOB_BASE, &gi);

    gi.Pin = 0xBCF1; gi.Mode = 1; gi.Field2 = 0; gi.Speed = 0;   /* GPIOD outputs */
    HAL_GPIO_Init(GPIOD_BASE, &gi);

    gi.Pin = 0x400E; gi.Mode = 0; gi.Field2 = 0;                 /* GPIOD inputs */
    HAL_GPIO_Init(GPIOD_BASE, &gi);

    gi.Pin = 0x100; gi.Mode = GPIO_MODE_AF_C; gi.Field2 = 1;     /* GPIOC AF idx 1 */
    HAL_GPIO_Init(GPIOC_BASE, &gi);

    gi.Pin = 0x800; gi.Mode = 0; gi.Field2 = 0;                  /* GPIOA input */
    HAL_GPIO_Init(GPIOA_BASE, &gi);

    gi.Pin = 0x9000; gi.Mode = 1; gi.Field2 = 0; gi.Speed = 0;   /* GPIOA outputs */
    HAL_GPIO_Init(GPIOA_BASE, &gi);

    gi.Pin = 0x400; gi.Mode = GPIO_MODE_AF_C; gi.Field2 = 2;     /* GPIOC AF idx 2 */
    HAL_GPIO_Init(GPIOC_BASE, &gi);
}

/* PC0 / PC1 sense lines: return 1 when the pin reads LOW, else 0 (OEM
 * gpio_pc0_is_low 0x08040350 / gpio_pc1_is_low 0x08040368 — the two button-state
 * bytes of the BLE 0x5568 read). The OEM calls HAL_GPIO_ReadPin (1 = high) and
 * inverts the result through the branchless clz/lsr#5 "== 0" idiom. */
int gpio_pc0_is_low(void)
{
    return HAL_GPIO_ReadPin(GPIOC_BASE, 0x1) == 0;   /* PC0 */
}

int gpio_pc1_is_low(void)
{
    return HAL_GPIO_ReadPin(GPIOC_BASE, 0x2) == 0;   /* PC1 */
}
