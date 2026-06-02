#include "powerbankware.h"

/*
 * HAL / board bring-up for the STM32F091 powerbankware.
 *
 * Faithful translations of the bring-up cluster called from main() via
 * hal_bringup:
 *   hal_bringup       = FUN_080114dc   (flash/vector-remap/clock-enable orchestrator)
 *   hal_init          = FUN_080192c4   (HAL_Init: prefetch + tick + MspInit)
 *   tick_state_reset  = FUN_08014ac8   (clear the software ms-tick flag/counter)
 *   board_init        = FUN_08011f2c   (GPIO board config + peripheral sub-inits)
 *
 * Widths, constants, struct field offsets and call arity below are
 * disasm-confirmed against the OEM image.
 */

/* --- hal_init deeper HAL leaves (own passes) --- */
extern int  FUN_080192f6(int tick_prio); /* HAL_InitTick */
extern void FUN_080192ec(void);          /* HAL_MspInit (empty weak stub) */

/* --- board_init deeper leaves (own passes) --- */
extern void FUN_08012188(void);          /* peripheral sub-init */
extern void FUN_0801647c(void);          /* peripheral sub-init */
extern void FUN_08008804(void);          /* peripheral sub-init */
extern void FUN_08010d90(void);          /* peripheral sub-init */
extern void FUN_0800e910(void);          /* peripheral sub-init */
extern void FUN_0800a310(void);          /* peripheral sub-init */
extern void FUN_0800e32c(void);          /* peripheral sub-init */
extern void FUN_08019c4c(int irqn, int prio, int subprio); /* HAL_NVIC_SetPriority */
extern void FUN_08019c76(int irqn);                        /* HAL_NVIC_EnableIRQ */

/*
 * hal_init — OEM FUN_080192c4 (HAL_Init).
 * Enable the flash prefetch buffer, configure the 1 ms SysTick, then run the
 * (empty) MSP init. Returns HAL_OK (0).
 */
int hal_init(void)
{
    *(volatile uint32_t *)0x40022000 |= 0x10u;   /* FLASH_ACR: PRFTBE (prefetch) */
    FUN_080192f6(0);                             /* HAL_InitTick(TICK_INT_PRIORITY=0) */
    FUN_080192ec();                              /* HAL_MspInit */
    return 0;
}

/*
 * tick_state_reset — OEM FUN_08014ac8.
 * Clear the software millisecond-tick flag byte (0x2000077c, shared with
 * delay.c) and its 16-bit companion counter (0x20000778).
 */
void tick_state_reset(void)
{
    *(volatile uint8_t  *)0x2000077c = 0;   /* SysTick 1 ms / periodic flag byte */
    *(volatile uint16_t *)0x20000778 = 0;
}

/*
 * board_init — OEM FUN_08011f2c.
 *
 * Enable the GPIOA/B/C/F port clocks, drive the initial output levels and
 * configure every board pin, run the per-peripheral sub-inits, recover the
 * I2C bus if SDA is held low, then enable the EXTI4_15 interrupt.
 */
void board_init(void)
{
    volatile uint32_t * const RCC_AHBENR = (volatile uint32_t *)(0x40021000 + 0x14);
    volatile uint8_t  * const tick_flag  = (volatile uint8_t  *)0x2000077c;

    /* RCC AHBENR: GPIOA(17)/GPIOB(18)/GPIOC(19)/GPIOF(22) clock enable, each
     * with the HAL clock-enable read-back. */
    *RCC_AHBENR |= 0x00020000u; (void)(*RCC_AHBENR & 0x00020000u);
    *RCC_AHBENR |= 0x00040000u; (void)(*RCC_AHBENR & 0x00040000u);
    *RCC_AHBENR |= 0x00080000u; (void)(*RCC_AHBENR & 0x00080000u);
    *RCC_AHBENR |= 0x00400000u; (void)(*RCC_AHBENR & 0x00400000u);

    /* Initial output levels. */
    gpio_bit_write(0x48000000u, 0x8180, 1);   /* GPIOA 7,8,15 high */
    gpio_bit_write(0x48000000u, 0x1200, 0);   /* GPIOA 9,12 low */
    gpio_bit_write(0x48000400u, 0x3001, 1);   /* GPIOB 0,12,13 high */
    gpio_bit_write(0x48000400u, 0x8e86, 0);   /* GPIOB 1,2,7,9,10,11,15 low */

    /* Pin configuration. The OEM reuses a single init struct across calls, so
     * fields it does not re-write inherit the previous call's value (e.g. the
     * 0x4140 input config below keeps speed=3 from the 0x9380 output config). */
    gpio_pin_cfg_t cfg;
    mem_set(&cfg, 0, sizeof cfg);

    cfg.pin_mask = 0x2000; cfg.mode = 0; cfg.pupd = 0;
    gpio_pin_config((uint32_t *)0x48000800u, &cfg);   /* GPIOC13 input */

    cfg.pin_mask = 0x0c00; cfg.mode = 0; cfg.pupd = 0;
    gpio_pin_config((uint32_t *)0x48000000u, &cfg);   /* GPIOA10,11 input */

    cfg.pin_mask = 0x9380; cfg.mode = 1; cfg.pupd = 0; cfg.speed = 3;
    gpio_pin_config((uint32_t *)0x48000000u, &cfg);   /* GPIOA7,8,9,12,15 output, vhigh */

    cfg.pin_mask = 0x4140; cfg.mode = 0; cfg.pupd = 0; /* speed inherits 3 */
    gpio_pin_config((uint32_t *)0x48000400u, &cfg);   /* GPIOB6,8,14 input */

    cfg.pin_mask = 0xbe87; cfg.mode = 1; cfg.pupd = 0; cfg.speed = 3;
    gpio_pin_config((uint32_t *)0x48000400u, &cfg);   /* GPIOB output, vhigh */

    FUN_08012188();
    FUN_0801647c();
    FUN_08008804();
    FUN_08010d90();
    FUN_0800e910();
    FUN_0800a310();

    /* I2C bus recovery: while SDA (PB14) is held low, pulse SCL (PB13) ten
     * times at ~1 ms/edge (gated on the software tick flag), re-checking SDA. */
    if (!gpio_bit_read(0x48000400u, 0x4000)) {
        do {
            for (uint16_t i = 0; i < 10; i++) {
                gpio_bit_write(0x48000400u, 0x2000, 0);
                while ((*tick_flag & 1) == 0) { }
                *tick_flag &= (uint8_t)~1u;
                gpio_bit_write(0x48000400u, 0x2000, 1);
                while ((*tick_flag & 1) == 0) { }
                *tick_flag &= (uint8_t)~1u;
            }
        } while (!gpio_bit_read(0x48000400u, 0x4000));
    }

    FUN_0800e32c();
    FUN_08019c4c(7, 3, 0);   /* HAL_NVIC_SetPriority(EXTI4_15_IRQn, 3, 0) */
    FUN_08019c76(7);         /* HAL_NVIC_EnableIRQ(EXTI4_15_IRQn) */
}

/*
 * hal_bringup — OEM FUN_080114dc.
 *
 * Earliest hardware bring-up, called first from main(). Enable the flash
 * prefetch buffer, relocate the 48-entry vector table into SRAM and remap SRAM
 * to address 0 (Cortex-M0 has no VTOR), enable the SYSCFG and PWR peripheral
 * clocks (each with the HAL clock-enable read-back), then run the
 * per-peripheral init helpers.
 */
void hal_bringup(void)
{
    *(volatile uint32_t *)0x40022000 |= 0x10u;        /* FLASH_ACR: PRFTBE (prefetch) */

    /* Cortex-M0 has no VTOR: copy the 48-entry vector table to SRAM start, then
     * point the 0x00000000 region at SRAM via SYSCFG. Vectors are at image+0x28. */
    block_copy((void *)0x20000000, (const void *)0x08008028, 0xc0);
    *(volatile uint32_t *)0x40010000 |= 3u;           /* SYSCFG_CFGR1: MEM_MODE = SRAM */

    hal_init();

    *(volatile uint32_t *)(0x40021000 + 0x18) |= 1u;             /* RCC_APB2ENR: SYSCFGEN */
    (void)(*(volatile uint32_t *)(0x40021000 + 0x18) & 1u);
    *(volatile uint32_t *)(0x40021000 + 0x1c) |= 0x10000000u;    /* RCC_APB1ENR: PWREN */
    (void)(*(volatile uint32_t *)(0x40021000 + 0x1c) & 0x10000000u);

    clock_rtc_init();
    tick_state_reset();
    iwdg_init();
    flash_lock();                                     /* FUN_0801a298 */
    board_init();
}
