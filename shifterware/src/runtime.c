/* runtime.c — Cortex-M0 compiler-runtime helpers (no libgcc with our
 * `-nostdlib` build). The OEM image carries its own implementations of
 * these because it links without libgcc too. */

#include <stdint.h>

/* OEM @ 0x08005D40 (44 B). Classic restoring 32-bit-unsigned-divide
 * loop: walk shift positions from 31 down to 0, and at each position
 * test whether the divisor (shifted up) still fits inside the
 * remaining dividend; if so subtract it and set the corresponding
 * quotient bit. Returns quotient only (no remainder out-parameter, so
 * this is `__aeabi_uidiv` rather than `__aeabi_uidivmod`).
 *
 * Cortex-M0 has no UDIV instruction, so GCC emits a call to this
 * helper for every C-level `/` and `%` on unsigned 32-bit values. We
 * link `-nostdlib`, so we must provide it ourselves; if we don't, the
 * linker will fail the first time real code (e.g. `usart_init`, which
 * computes `PCLK / baud`) goes live. */
uint32_t __aeabi_uidiv(uint32_t dividend, uint32_t divisor)
{
    uint32_t quotient = 0u;
    for (int shift = 31; shift >= 0; shift--) {
        if (divisor <= (dividend >> (uint32_t)shift)) {
            dividend -= (divisor << (uint32_t)shift);
            quotient += (1u << (uint32_t)shift);
        }
    }
    return quotient;
}
