#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SysTick-driven millisecond countdown, decremented once per tick by
 * systick_tick() until it reaches zero. The OEM places this at SRAM
 * 0x20000010. Used by delay-style helpers elsewhere in the bootloader. */
extern volatile uint32_t g_systick_countdown;

void systick_tick(void);
void SysTick_Handler(void);

/* Busy-wait for `ms` milliseconds, driven by SysTick decrementing
 * `g_systick_countdown`. Assumes `SysTick_Config` has already been
 * run with a 1 ms reload value (so each tick fires once per ms).
 * Sole caller in the OEM is `main` at `0x080002E2` with `ms = 250`,
 * inserting a 250 ms pause after the image-sync chain. */
void mdelay(uint32_t ms);

/* Configure SysTick for a 1 ms tick at HCLK = 48 MHz: reload = 47999,
 * CLKSOURCE/TICKINT/ENABLE all set, IRQ priority raised to 0
 * (highest CM0). Called once from `main` before the Modbus loop
 * comes up. */
void boot_init_systick(void);

#ifdef __cplusplus
}
#endif

#endif
