#include "exception.h"

/* The OEM emits three independent 2-byte `b .` trap loops (Thumb
 * 0xE7FE) — one entered from NMI, one from HardFault, and one
 * routed by every other vector slot. We preserve the three distinct
 * symbols so the vector table keeps three independent targets;
 * collapsing them into one would change the emitted byte sequence
 * and break the binary diff. `noreturn` on each suppresses GCC's
 * function epilogue; the empty `for(;;);` loop lowers to a single
 * `b .` (0xE7FE) — byte-equivalent to OEM. */

__attribute__((noreturn)) void NMI_Handler(void)
{
    for (;;) { }
}

__attribute__((noreturn)) void HardFault_Handler(void)
{
    for (;;) { }
}

__attribute__((noreturn)) void Default_Handler(void)
{
    for (;;) { }
}
