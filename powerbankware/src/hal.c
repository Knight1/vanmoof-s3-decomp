#include "powerbankware.h"

/*
 * hal_bringup — OEM FUN_080114dc.
 *
 * Earliest hardware bring-up, called first from main(). Enable the flash prefetch
 * buffer, relocate the 48-entry vector table into SRAM and remap SRAM to address 0
 * (Cortex-M0 has no VTOR), enable the SYSCFG and PWR peripheral clocks (each with
 * the HAL clock-enable read-back), then run the per-peripheral init helpers.
 * Cross-checked against the OEM machine code.
 */

/* Per-peripheral init helpers (own passes). */
extern void FUN_080192c4(void);   /* interrupt / NVIC setup */
extern void FUN_080136c0(void);
extern void FUN_08014ac8(void);
extern void FUN_08013820(void);
extern void FUN_08011f2c(void);

void hal_bringup(void)
{
    *(volatile uint32_t *)0x40022000 |= 0x10u;        /* FLASH_ACR: PRFTBE (prefetch) */

    /* Cortex-M0 has no VTOR: copy the 48-entry vector table to SRAM start, then
     * point the 0x00000000 region at SRAM via SYSCFG. Vectors are at image+0x28. */
    block_copy((void *)0x20000000, (const void *)0x08008028, 0xc0);
    *(volatile uint32_t *)0x40010000 |= 3u;           /* SYSCFG_CFGR1: MEM_MODE = SRAM */

    FUN_080192c4();

    *(volatile uint32_t *)(0x40021000 + 0x18) |= 1u;             /* RCC_APB2ENR: SYSCFGEN */
    (void)(*(volatile uint32_t *)(0x40021000 + 0x18) & 1u);
    *(volatile uint32_t *)(0x40021000 + 0x1c) |= 0x10000000u;    /* RCC_APB1ENR: PWREN */
    (void)(*(volatile uint32_t *)(0x40021000 + 0x1c) & 0x10000000u);

    FUN_080136c0();
    FUN_08014ac8();
    FUN_08013820();
    flash_lock();                                     /* FUN_0801a298 */
    FUN_08011f2c();
}
