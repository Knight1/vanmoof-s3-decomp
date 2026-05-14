/* TI ARM Compiler runtime hooks that the OEM image kept at their
 * weak-default values. These are intentionally trivial: replicate
 * the exact byte sequence the upstream weak implementations emit,
 * so the OEM image and our reconstruction stay byte-equivalent at
 * the call sites. */

/* `_system_pre_init` — TI CGT compiler-runtime calls this before
 * `_auto_init_*` (cinit). Returning nonzero enables cinit; zero
 * skips it. The weak default in TI's RTS library is exactly
 * `return 1;`, which compiles to `movs r0, #1; bx lr` (4 bytes).
 * The OEM kept that default. */
int _system_pre_init(void)
{
    return 1;
}

/* `_exit` — TI CGT runtime calls this after `main` returns to
 * terminate the program. The weak default is a trap loop; the OEM
 * kept it. We render it as `nop; b .` to match the OEM byte
 * sequence (`bf00 e7fe`) rather than the canonical `b .` (`e7fe`)
 * alone — the leading nop comes from the TI CCS startup template
 * and is what the OEM image carries. */
__attribute__((naked, noreturn)) void _exit(int status)
{
    (void)status;
    __asm__ volatile (
        "nop      \n\t"
        "1: b 1b  \n\t"
    );
}
