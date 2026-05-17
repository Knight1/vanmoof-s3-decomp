/* bleboot startup chain — `Reset_Handler` (vector_table[1]) and the
 * tail-called `ResetISR_body`.
 *
 * Two halves of TI's stock GCC startup pattern. Upstream source lives
 * at `source/ti/devices/cc13x2_cc26x2/startup_files/startup_gcc.c` in
 * the SimpleLink CC13x2/CC26x2 SDK 3.40.00.02 (April 2020 — the TI
 * release closest to bleboot's `Apr 23 2020` build date). Linked,
 * not vendored.
 *
 * `Reset_Handler` (OEM @ 0x57126, 10 B): vector-table entry. Calls
 *   `SetupTrimDevice` (silicon trim), then tail-calls `ResetISR_body`.
 *   With `ResetISR_body` annotated `noreturn`, GCC tail-call-optimises
 *   the second call to a `b.w` and skips the epilogue — same
 *   instruction shape as the OEM (`push {r3, lr}; bl SetupTrimDevice;
 *   b.w ResetISR_body`).
 *
 * `ResetISR_body` (OEM @ 0x56DD8, 52 B): C-runtime entry. The only
 *   architecturally-mandated inline-asm fragment is the `msr msp`
 *   reset (Cortex-M has no C-level intrinsic for it without pulling
 *   in CMSIS). Everything else — FPU enable, cinit dispatch, `main`,
 *   `_exit` — is plain C. */

#include <stdint.h>

extern int  _system_pre_init(void);
extern void _auto_init_table(void);
extern int  main(int argc);
extern void _exit(int status) __attribute__((noreturn));
extern void SetupTrimDevice(void);
extern void ResetISR_body(void) __attribute__((noreturn));

__attribute__((noreturn))
void Reset_Handler(void)
{
    SetupTrimDevice();
    ResetISR_body();
}

__attribute__((noreturn))
void ResetISR_body(void)
{
    /* Reset MSP to top of SRAM — the only way to set MSP from C is
     * through an `msr` instruction. Single-line inline asm; everything
     * else in this function is portable C. */
    __asm__ volatile ("msr msp, %0" :: "r"(0x20014000u));

    /* Enable CP10/CP11 (FPU coprocessors) via SCB->CPACR. */
    *(volatile uint32_t *)0xE000ED88u |= 0xF00000u;
    __sync_synchronize();   /* lowered to `dsb` on Cortex-M */

    if (_system_pre_init()) {
        _auto_init_table();
    }
    main(0);
    _exit(1);
}
