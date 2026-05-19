#ifndef SHIFTERBOOT_CLOCK_H
#define SHIFTERBOOT_CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Clock-tree setup. Called from the vendor-stock `SetSysClock`
 * trampoline at `0x080005B6`, which is in turn called from the
 * vendor-stock `SystemInit` at `0x080005BE` during Reset_Handler.
 *
 * Brings the MM32F031 up to a 48 MHz SYSCLK with 1 flash wait state
 * and a /2 APB prescaler (PCLK = 24 MHz). See the function body in
 * `src/clock.c` for the per-bit derivation. */

void set_sysclock_to_48m(void);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_CLOCK_H */
