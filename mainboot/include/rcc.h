#ifndef RCC_H
#define RCC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pulse-reset every peripheral on every bus of the STM32F4 RCC and then
 * release them, returning 0. Used during very early boot to put all
 * peripherals into a known state before the loader configures the ones
 * it actually uses.
 *
 * The OEM order is APB1 → APB2 → AHB1 → AHB2 → AHB3. The function
 * returns int (always 0) — the caller appears to ignore the value but
 * the OEM ABI is preserved for byte-equivalence. */
int rcc_reset_all_peripherals(void);

/* The OEM has a 2-byte `bx lr` stub right before rcc_reset_all_peripherals
 * (at 0x080005D0) that this function calls between writing the last reset
 * register and returning. It does nothing observable — likely a Muco
 * placeholder hook intended for the integrator to fill in a delay or
 * memory barrier. We keep it as a real function so the OEM call survives
 * byte-equivalence. */
void rcc_post_reset_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* RCC_H */
