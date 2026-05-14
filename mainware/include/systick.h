#ifndef MAINWARE_SYSTICK_H
#define MAINWARE_SYSTICK_H

#include <stdint.h>

/* Muco-runtime SysTick globals. Same shape and same g_systick_step
 * address as in mainboot; only g_systick_counter has moved (mainware
 * places its bss at a different SRAM offset). The step byte is the
 * SysTick increment per tick — initialised to 1, but the runtime can
 * raise it for low-power "skip ahead" modes. */
extern volatile uint32_t g_systick_counter;  /* SRAM 0x20009704 */
extern volatile uint8_t  g_systick_step;     /* SRAM 0x20000014 */

void systick_tick(void);

#endif
