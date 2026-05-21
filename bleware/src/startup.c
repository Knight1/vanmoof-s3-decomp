/* startup.c — bleware Reset_Handler + cinit walker.
 *
 * The OEM at `Reset_Handler` (flash 0x0001F590, 70 B) fuses what
 * bleboot keeps as two functions (`Reset_Handler` + `ResetISR_body`).
 * The chain is:
 *
 *   1. Enable FPU: SCB->CPACR |= 0xF00000 (CP10 + CP11).
 *   2. Materialise MSP = (_stack_base + _stack_size) & ~7
 *      = (0x20013A00 + 0x600) & ~7 = 0x20014000 (top of SRAM).
 *      Save the resolved MSP to *(0x20005B30) — a runtime "stack top"
 *      shadow the OEM keeps for stack-overflow checks.
 *   3. The OEM checks a function-pointer at pool word 0x0001F5E4
 *      (= 0x00027773 → thunk → SetupTrimDevice @ 0x0001878C). When
 *      non-null (always, in this build), the indirect call runs;
 *      otherwise the inline SetupTrimDevice call below would run.
 *   4. cinit_walker() — TI CGT C runtime init: walks the cinit table
 *      (BSS-fill / .data-copy entries dispatched through a 4-entry
 *      handler table) and the auto-init constructor pass.
 *   5. main(argc, argv) — argc/argv read from the global at 0x00026488
 *      by main_trampoline (the actual main is at 0x0001CFEC).
 *   6. _exit(code) — ROM thunk at flash 0x00027A20 → ROM 0x1002F7B0.
 *   7. `b .` trap (unreachable).
 *
 * For this skeleton build we collapse the OEM's indirect SetupTrimDevice
 * call into a direct one. The skeleton produces a working build; a
 * later byte-equivalence pass will reintroduce the indirection.
 */

#include "bleware.h"
#include "cc2642r1.h"

#include <stdint.h>

/* Linker-exported stack top. */
extern uint32_t _estack;

/* TI-RTOS / SimpleLink BIOS_start lives in ROM. Declared `noreturn`
 * so GCC can tail-call optimise. The decomp at 0x1002EAA4 confirms
 * this. */
extern void BIOS_start(void) __attribute__((noreturn));

/* `_exit` is a small thunk to ROM 0x1002F7B0. Provide a local stub
 * that traps so the skeleton link is clean; replace with the real
 * thunk body when we decode it. */
__attribute__((noreturn, weak))
void _exit(int code)
{
    (void)code;
    for (;;) {
        /* trap until reset */
    }
}

__attribute__((noreturn))
void Reset_Handler(void)
{
    /* Enable FPU coprocessors CP10 + CP11 (the FPU enable bits in
     * SCB->CPACR are bits 20..23). */
    SCB_CPACR |= 0xF00000u;
    __asm__ volatile ("dsb sy; isb sy" ::: "memory");

    /* The OEM additionally writes the resolved MSP value to a runtime
     * shadow at SRAM 0x20005B30. Skipped here — that shadow is only
     * read by TI's stack-overflow check, which we'll wire up after
     * the kernel modules land. The CPU's MSP itself is loaded from
     * vector slot 0 (the hardware loads it on reset). */

    /* Silicon trim. */
    SetupTrimDevice();

    /* C runtime init. */
    cinit_walker();

    /* User entry. */
    int rc = main(0, (char **)0);
    _exit(rc);
}
