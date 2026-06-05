#include <stdint.h>

#include "watchdog.h"

/* Descriptor-driven 32-bit register write (OEM wdg_reg_write_from_desc,
 * 0x08026DB4): store desc[3] (word index 3 = byte +0xC) to the MMIO address in
 * desc[0]. The runtime descriptor @ 0x20009728 (built by FUN_08031444) makes the
 * sole use a WWDG counter reload: desc[0] = WWDG_CR (0x40002C00), desc[3] = 0x7F. */
uint32_t wdg_reg_write_from_desc(uint32_t *desc)
{
    *(volatile uint32_t *)desc[0] = desc[3];
    return 0;
}

/* Refresh the window watchdog (OEM watchdog_kick, 0x080314D8): a thunk applying
 * the descriptor at 0x20009728 — reloads WWDG_CR with 0x7F. The OEM tail-calls
 * the worker; callers ignore the (always-0) return. */
void watchdog_kick(void)
{
    (void)wdg_reg_write_from_desc((uint32_t *)0x20009728u);
}
