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

#ifdef __cplusplus
}
#endif

#endif
