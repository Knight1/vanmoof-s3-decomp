/* watchdog.c — independent watchdog (IWDG, LSI ~40 kHz). */

#include "watchdog.h"
#include "mm32f031.h"

#define LSI_HZ          40000u

void watchdog_init(uint32_t timeout_ms)
{
    IWDG->KR = IWDG_KEY_WRITE;

    /* Pick the smallest prescaler that fits the requested timeout into
     * the 12-bit reload register. */
    uint8_t  pr  = 0u;          /* /4   */
    uint32_t div = 4u;
    while (pr < 6u) {
        const uint32_t reload = ((uint32_t)timeout_ms * LSI_HZ) / (div * 1000u);
        if (reload <= 0xFFFu) {
            IWDG->PR  = pr;
            IWDG->RLR = reload ? reload : 1u;
            break;
        }
        pr++;
        div <<= 1;
    }
    if (pr >= 6u) {
        IWDG->PR  = 6u;
        IWDG->RLR = 0xFFFu;
    }

    IWDG->KR = IWDG_KEY_RELOAD;
    IWDG->KR = IWDG_KEY_ENABLE;
}

void watchdog_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}
