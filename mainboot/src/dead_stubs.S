/* dead_stubs.S — mainboot
 *
 * Functions that the Muco bootloader keeps as real symbols (with real
 * call sites) but whose bodies have been disabled at build time. The
 * canonical fingerprint is: a guard literal of zero, a CBZ that always
 * fires, a body of preloaded arguments, and a wide NOP where a BL used
 * to be. The arguments survive in the literal pool, so the original
 * intent (which callee, which context pointer) can sometimes be
 * recovered later from cross-references — that is why we preserve the
 * function in .S form rather than collapsing it to an empty C function.
 */

    .syntax unified
    .cpu    cortex-m4
    .thumb

/* ---------------------------------------------------------------------
 * dispatch_disabled_stub
 *   OEM: FUN_08000204, 16 bytes function + 12 bytes literal pool.
 *
 *   if (*(uint32_t *)0x08000214 != 0) {       // literal == 0, always skips
 *       r1 = 0x20000200;                      // SRAM context pointer
 *       r0 = 0x080055EC;                      // function pointer in flash
 *       // BL <handler> here, now NOP'd out
 *   }
 *
 *   Called twice from inside the main bootloader loop. Both call sites
 *   appear to dispatch the same disabled hook — almost certainly a
 *   compile-time-removable diagnostic / event helper. We keep it
 *   verbatim because the OEM emits real call instructions to this
 *   address.
 * ------------------------------------------------------------------- */
    .section .text.dispatch_disabled_stub, "ax", %progbits
    .global  dispatch_disabled_stub
    .thumb_func
dispatch_disabled_stub:
    push    {r3, lr}
    ldr     r3, .Ldis_pool0         /* guard: *0x08000214, baked to 0 */
    cbz     r3, .Ldis_exit
    ldr     r1, .Ldis_pool1         /* = 0x20000200 (SRAM ctx) */
    ldr     r0, .Ldis_pool2         /* = 0x080055EC (handler) */
    nop.w                           /* was BL <handler> in pre-strip build */
.Ldis_exit:
    pop     {r3, pc}

    .align  2
.Ldis_pool0:
    .word   0
.Ldis_pool1:
    .word   0x20000200
.Ldis_pool2:
    .word   0x080055EC
    .size   dispatch_disabled_stub, .-dispatch_disabled_stub
