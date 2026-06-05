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

/* Bring up the window watchdog: build the descriptor @ 0x20009728 and program
 * the WWDG hardware (OEM watchdog_init, 0x08031444). */
void watchdog_init(void);

/* WWDG hardware init from the descriptor (OEM 0x08026D8A); 0 ok / 1 if NULL. */
int wwdg_hw_init(uint32_t *desc);

/* Enable the WWDG peripheral clock (OEM 0x08031474). */
void wwdg_clk_enable(uint32_t *desc);

#endif
