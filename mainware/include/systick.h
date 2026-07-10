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

/* Read the free-running tick counter — milliseconds since boot (1 ms/tick).
 * OEM systick_now at 0x080232F8; the timing reference used across the app
 * (e.g. by systick_delay and the per-subsystem poll throttles). */
uint32_t systick_now(void);

/* Busy-wait `ticks` SysTick periods, rounded up by one g_systick_step so the
 * caller gets at least `ticks` whole periods. 0xFFFFFFFF blocks forever.
 * OEM systick_delay at 0x08023304. */
void systick_delay(uint32_t ticks);

/* Gate the SysTick interrupt by clearing/setting SysTick CTRL.TICKINT (bit 1,
 * at 0xE000E010). OEM systick_irq_disable 0x0802332C / systick_irq_enable
 * 0x0802333C — used around the stop/low-power transitions. */
void systick_irq_disable(void);
void systick_irq_enable(void);

#endif
