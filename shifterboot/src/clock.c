/* clock.c — MM32F031 system clock bring-up.
 *
 * Single function `set_sysclock_to_48m` decomp'd from
 * shifterboot.bin at OEM @ 0x0800054C (106 B). Called via the
 * 8-byte vendor-stock `SetSysClock` trampoline at 0x080005B6 →
 * which is invoked at the tail of `SystemInit` (0x080005BE) during
 * Reset_Handler.
 *
 * NOT byte-identical to MindMotion's published `SetSysClockTo48M`
 * — the OEM here skips the explicit PLLMUL / PLLSRC / PLLON dance.
 * Instead it:
 *
 *   1. Enables HSI (`RCC->CR.HSION = 1`), waits for HSIRDY.
 *   2. Clears two MM32F031-specific RCC->CR bits (bit 20 and bit 2)
 *      that aren't documented in the standard STM32F0 RM but that
 *      the shifterware-side `rcc_get_clocks_freq` confirms gate the
 *      "PLL gives 48 MHz vs 72 MHz" behaviour (clear bit 20 → 48
 *      MHz). Bit 2's role is still TBD; clearing it is part of the
 *      defaults sequence.
 *   3. Writes `RCC->CFGR = 0x400` — clears the whole register and
 *      sets PPRE[2:0]=100 (APB prescaler /2). HPRE stays at 0
 *      (AHB /1) and SW stays at 00 (HSI for now).
 *   4. Writes `FLASH->ACR = 0x11` — prefetch buffer enabled, latency
 *      = 1 wait state (required for 24..48 MHz).
 *   5. Clears the SW field, then writes SW=10 (clock source = PLL).
 *      Because PLLMUL/PLLSRC were zeroed and PLL was never explicitly
 *      enabled, the MM32F031 routes a 48 MHz internal source through
 *      "SW=PLL" — confirmed empirically by `rcc_get_clocks_freq`
 *      reporting SYSCLK = 48 MHz once SWS reaches 10.
 *   6. Spins until `SWS == 10` (PLL is the active clock).
 *
 * The OEM materialises the literal 0x400 in step 3 via
 * `asrs r1, r2, #0x14` on the RCC base in r2 (`0x40021000 >> 20 =
 * 0x400`) — a peephole that saves a literal-pool word. We don't
 * try to reproduce that; -Os picks its own way to load 0x400.
 */

#include "clock.h"

#include <stdint.h>

#define RCC_BASE            (0x40021000u)
#define FLASH_BASE          (0x40022000u)

#define RCC_CR              (*(volatile uint32_t *)(RCC_BASE + 0x00u))
#define RCC_CFGR            (*(volatile uint32_t *)(RCC_BASE + 0x04u))
#define FLASH_ACR           (*(volatile uint32_t *)(FLASH_BASE + 0x00u))

#define RCC_CR_HSION        (1u <<  0)
#define RCC_CR_HSIRDY       (1u <<  1)
#define RCC_CR_BIT2         (1u <<  2)     /* MM32F031-specific; role TBD */
#define RCC_CR_BIT20        (1u << 20)     /* MM32F031-specific; 0 → 48 MHz, 1 → 72 MHz */

#define RCC_CFGR_SW_Msk     (0x3u << 0)
#define RCC_CFGR_SW_PLL     (0x2u << 0)
#define RCC_CFGR_SWS_Msk    (0x3u << 2)
#define RCC_CFGR_SWS_PLL    (0x2u << 2)
#define RCC_CFGR_PPRE_DIV2  (0x4u << 8)    /* APB prescaler = /2 */

#define FLASH_ACR_LATENCY_1WS (1u << 0)
#define FLASH_ACR_PRFTBE      (1u << 4)

void set_sysclock_to_48m(void)
{
    RCC_CR = RCC_CR | RCC_CR_HSION;
    while ((RCC_CR & RCC_CR_HSIRDY) == 0u) {
        /* wait for HSIRDY */
    }

    RCC_CR = RCC_CR & ~RCC_CR_BIT20;
    RCC_CR = RCC_CR & ~RCC_CR_BIT2;

    RCC_CFGR = RCC_CFGR_PPRE_DIV2;

    FLASH_ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_1WS;

    RCC_CFGR = RCC_CFGR & ~RCC_CFGR_SW_Msk;
    RCC_CFGR = RCC_CFGR | RCC_CFGR_SW_PLL;

    while ((RCC_CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) {
        /* wait until PLL is the active SYSCLK source */
    }
}
