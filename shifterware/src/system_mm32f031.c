/* system_mm32f031.c — low-level CPU/clock init.
 *
 * Called from Reset_Handler before main(). Brings the MCU up to 48 MHz
 * off the internal HSI through the PLL (HSI/2 * 12 = 48 MHz), enables
 * the flash prefetch and one wait state, and leaves all peripherals
 * gated off so per-module init explicitly turns them on.
 */

#include "shifter.h"
#include "mm32f031.h"

void SystemInit(void)
{
    /* 1) Make sure HSI is running. */
    RCC->CR |= RCC_CR_HSION_Msk;
    while ((RCC->CR & RCC_CR_HSIRDY_Msk) == 0u) {
        /* spin */
    }

    /* 2) One wait state + prefetch for >24 MHz CPU clock (RM §3.5.1). */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk)
               | FLASH_ACR_LATENCY_1WS
               | FLASH_ACR_PRFTBE_Msk;

    /* 3) PLL = HSI/2 * 12 = 48 MHz. PLL must be off while reconfiguring. */
    RCC->CR &= ~RCC_CR_PLLON_Msk;
    RCC->CFGR = (RCC->CFGR & ~(0xFu << 18))
              | RCC_CFGR_PLLSRC_HSI_DIV2
              | RCC_CFGR_PLLMUL_12;
    RCC->CR |= RCC_CR_PLLON_Msk;
    while ((RCC->CR & RCC_CR_PLLRDY_Msk) == 0u) {
        /* spin */
    }

    /* 4) Select PLL as system clock. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) {
        /* spin */
    }

    /* 5) SYSCFG is always-on so EXTI / pin remap work later. */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN_Msk;
}
