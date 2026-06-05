#ifndef MAINWARE_WATCHDOG_H
#define MAINWARE_WATCHDOG_H

#include <stdint.h>

/* Refresh the window watchdog (OEM watchdog_kick, 0x080314D8): reloads WWDG_CR
 * (0x40002C00) with 0x7F via the descriptor at SRAM 0x20009728. Called each
 * super-loop iteration and during slow flash erase/program. */
void watchdog_kick(void);

/* Descriptor-driven 32-bit register write (OEM wdg_reg_write_from_desc,
 * 0x08026DB4): store desc[3] to the MMIO address held in desc[0]. */
uint32_t wdg_reg_write_from_desc(uint32_t *desc);

#endif
