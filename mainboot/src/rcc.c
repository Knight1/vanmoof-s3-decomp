#include <stdint.h>

#include "rcc.h"
#include "stm32f4_regs.h"

void rcc_post_reset_hook(void)
{
}

int rcc_reset_all_peripherals(void)
{
    RCC_APB1RSTR = 0xFFFFFFFFu;
    RCC_APB1RSTR = 0u;
    RCC_APB2RSTR = 0xFFFFFFFFu;
    RCC_APB2RSTR = 0u;
    RCC_AHB1RSTR = 0xFFFFFFFFu;
    RCC_AHB1RSTR = 0u;
    RCC_AHB2RSTR = 0xFFFFFFFFu;
    RCC_AHB2RSTR = 0u;
    RCC_AHB3RSTR = 0xFFFFFFFFu;
    RCC_AHB3RSTR = 0u;

    rcc_post_reset_hook();

    return 0;
}
