#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Free-running millisecond counter, advanced by systick_tick() once per
 * SysTick interrupt. Used by delay/timeout helpers elsewhere in the
 * bootloader. OEM SRAM address: 0x2000083C (.bss). */
extern volatile uint32_t g_systick_counter;

/* Increment step applied each tick. The OEM initialises this to 1 in
 * .data at SRAM 0x20000014 (= a uint8_t = 1) — keeping it as a runtime
 * variable rather than a #define lets the integrator scale time (e.g.
 * for accelerated test). */
extern volatile uint8_t g_systick_step;

/* Adds g_systick_step to g_systick_counter. Called from
 * SysTick_Handler. */
void systick_tick(void);

/* Reads g_systick_counter (atomic on this MCU because the counter
 * is 32-bit and a single ldr is uninterruptible). */
uint32_t systick_get_count(void);

/* Busy-wait until at least `ticks` SysTick periods have elapsed
 * since the call. The implementation adds g_systick_step to the
 * requested count before comparing — that's the standard "round
 * up to the next tick boundary" guard so callers get a *minimum*
 * of `ticks` periods even when SysTick fires immediately after
 * the start sample. Passing 0xFFFFFFFF suppresses the adjustment
 * (sentinel used by the OEM as an effectively-infinite delay,
 * roughly the full 32-bit counter range). */
void systick_delay(uint32_t ticks);

#ifdef __cplusplus
}
#endif

#endif /* SYSTICK_H */
