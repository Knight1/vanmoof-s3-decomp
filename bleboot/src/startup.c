/* bleboot startup chain — `Reset_Handler` (vector_table[1]) and the
 * tail-called `ResetISR_body`.
 *
 * Two halves of TI's stock GCC startup pattern, compiled by GCC -Os
 * into physically separate functions in the OEM image and matched
 * here against the OEM byte layout. Upstream source lives at
 * `source/ti/devices/cc13x2_cc26x2/startup_files/startup_gcc.c` in
 * the SimpleLink CC13x2/CC26x2 SDK 3.40.00.02 (April 2020 — the TI
 * release closest to bleboot's `Apr 23 2020` build date). Linked,
 * not vendored.
 *
 * Reset_Handler @ 0x57126 (10 B): vector-table entry. Push frame,
 *   call `SetupTrimDevice` (silicon trim), then tail-call
 *   `ResetISR_body` via `b.w`. Naked + inline asm: a non-naked
 *   translation would emit `bl ResetISR_body; pop {r3, pc}` (12 B
 *   and a different shape — wrong size, wrong control flow).
 *
 * ResetISR_body @ 0x56DD8 (52 B): C-runtime entry. Loads MSP from
 *   the SRAM top (`0x20014000` = `vector_table[0]`), enables the
 *   FPU via `SCB->CPACR |= 0xF00000`, two `nop` barriers, then the
 *   standard TI CGT cinit chain:
 *     `_system_pre_init` -> `_auto_init_table` (when pre-init
 *     returned nonzero) -> `main(0)` -> `_exit(1)`.
 *   Naked so the literal pool can be pinned to the exact OEM layout
 *   (CPACR address at body+0x2C, MSP init at body+0x30 — opposite
 *   of source order because the first `ldr` reaches further).
 *
 * The 6 bytes at 0x57130..0x57135 in the OEM image (`bl HardFault_Handler;
 * pop {r3, pc}`) are GCC's defensive post-tail-call epilogue that the
 * upstream TI ResetISR emits because the upstream's tail-call target
 * (`localProgramStart`) isn't marked `noreturn`. Unreachable. Our
 * naked translation doesn't reproduce them; they sit outside Ghidra's
 * `Reset_Handler` function boundary and have no behavioural impact.
 *
 * The trailing 2-byte alignment pad before the literal pool uses the
 * T1 nop encoding (`46c0` = `mov r8, r8`) rather than the T2 narrow
 * nop (`bf00`) — TI CCS picks the legacy encoding for `.align` fill
 * inside a Thumb code region. Emitted via `.short 0x46c0`. */

/* `_system_pre_init`, `_auto_init_table`, `main`, `_exit`, and
 * `SetupTrimDevice` are resolved by other TUs in this tree (see
 * `setup_trim.c`, `auto_init.c`, `main.c`, `rts_hooks.c`); the
 * inline-asm `bl`s bind them at link time without needing C-level
 * prototypes. */

__attribute__((naked, noreturn))
void Reset_Handler(void)
{
    __asm__ volatile (
        "push   {r3, lr}              \n\t"
        "bl     SetupTrimDevice       \n\t"
        "b.w    ResetISR_body         \n\t"
    );
}

__attribute__((naked, noreturn))
void ResetISR_body(void)
{
    __asm__ volatile (
        "ldr    r0, .L_bleboot_msp_init        \n\t"
        "msr    msp, r0                        \n\t"
        "ldr    r1, .L_bleboot_cpacr_addr      \n\t"
        "ldr    r0, [r1]                       \n\t"
        "orr    r0, r0, #0xF00000              \n\t"
        "str    r0, [r1]                       \n\t"
        "nop                                   \n\t"
        "nop                                   \n\t"
        "bl     _system_pre_init               \n\t"
        "cbz    r0, .L_bleboot_skip_auto_init  \n\t"
        "bl     _auto_init_table               \n\t"
        ".L_bleboot_skip_auto_init:            \n\t"
        "movs   r0, #0                         \n\t"
        "bl     main                           \n\t"
        "movs   r0, #1                         \n\t"
        "bl     _exit                          \n\t"
        ".short 0x46c0                         \n\t"
        ".L_bleboot_cpacr_addr: .word 0xE000ED88  \n\t"
        ".L_bleboot_msp_init:   .word 0x20014000  \n\t"
    );
}
