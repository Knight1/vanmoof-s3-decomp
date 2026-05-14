/* startup_mm32f031.S — shifterboot
 *
 * Cortex-M0 startup for MM32F031F6U6, decompiled from
 * shifterboot.bin (SHA-512 see docs/hardware.md).
 *
 * Provenance: Reset_Handler, the vector table, and SystemInit are
 * byte-identical to MindMotion's stock pre-2021 AE-Team SDK
 * (startup_MM32F031x4x6_q.s + system_MM32F031x4x6_q.c). The only
 * bespoke pieces are (a) the 20-byte _cold_reset stub below that
 * replaces the Keil/ARMCC __main runtime entry, and (b) the
 * SysTick_Handler. See docs/hardware.md for references.
 *
 * Boot path observed in the OEM image:
 *
 *   1. CPU enters at the Reset vector (table slot 1) = Reset_Handler
 *      @ 0x0800060C. (stock MindMotion: sets MSP, optional SYSCFG
 *      remap-recovery if entered from the on-chip ROM bootloader,
 *      calls SystemInit, then bx's to __main — which in this build
 *      points at _cold_reset below instead of Keil's __main.)
 *   2. SystemInit @ 0x080005BE resets RCC->{CR,CFGR,CIR} to their
 *      power-on defaults and calls SetSysClock to configure the
 *      clock tree.
 *   3. _cold_reset @ 0x080000B4 sets MSP again, calls _init_data_bss
 *      to copy .data and zero .bss, then bx's to main.
 *
 * The cold reset stub is placed at the tail of the Cortex-M0 vector
 * table region (slots 45/46/47, which the CM0 NVIC doesn't use). This
 * is unusual placement but legal — VanMoof saves bytes by reusing the
 * unused-vector area for the custom __main replacement.
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ---------------------------------------------------------------------
 * Default_Handler
 *   OEM: FUN_08000644, 2 bytes. Single `b .` infinite loop, used by
 *   ~30 IRQ slots in the vector table and by the SysTick slot
 *   (slot 15) is NOT — that points at FUN_080014C2.
 *
 *   Eight more 2-byte trap stubs exist at 0x08000632..0x08000640, one
 *   per system-exception slot. They are byte-identical copies of this
 *   one; left pending so progress.md tracks the per-slot identity until
 *   each is explicitly checked.
 * ------------------------------------------------------------------- */
    .section .text.Default_Handler, "ax", %progbits
    .global  Default_Handler
    .thumb_func
Default_Handler:
    b       .
    .size   Default_Handler, .-Default_Handler


/* ---------------------------------------------------------------------
 * _cold_reset (and its tail, the thunk_main trampoline)
 *   OEM: 0x080000B4..0x080000C7, 20 bytes (12 B code + 8 B literal pool).
 *
 *   Ghidra split this into Reset_Handler's stray-prefix (the first
 *   three instructions, mis-grouped into the warm-reset handler at
 *   0x0800060C) and a 4-byte `thunk_main` (the `ldr r0,[pc]; bx r0`
 *   trampoline). They are one logical block.
 *
 *   The stub is reached *after* Reset_Handler's tail bx — i.e., this is
 *   not directly pointed at by the vector table. The OEM startup
 *   appears to be a two-stage design: Reset_Handler does early MMIO
 *   setup, then funnels into the cold stub for the standard CMSIS
 *   .data/.bss bring-up + jump-to-main.
 * ------------------------------------------------------------------- */
    .section .text._cold_reset, "ax", %progbits
    .global  _cold_reset
    .thumb_func
_cold_reset:
    ldr     r0, =0x200006F8         /* initial MSP, mirrors vector slot 0 */
    mov     sp, r0
    bl      _init_data_bss          /* OEM FUN_08001764 */
    /* The OEM emits an explicit pool-thunk here rather than a `bl main`
     * or `bx pc`: it loads main's address from a literal pool word and
     * `bx`-es through it, leaving LR pointing back into this stub so a
     * stray `bx lr` from main would re-enter the thunk and re-jump to
     * main forever (effectively making main no-return without the
     * compiler needing to know that). Decompiled-as-named by Ghidra as
     * `thunk_main` because it matches the Thumb-mode thunk pattern. */
    .global  thunk_main
    .thumb_func
thunk_main:
    ldr     r0, =main               /* = 0x080001D9 (main, thumb bit set) */
    bx      r0
    .size   thunk_main, .-thunk_main
    .size   _cold_reset, .-_cold_reset

/* The two literal-pool words `ldr r0,=...` emits sit at:
 *   0x080000C0 : .word main          (0x080001D9)
 *   0x080000C4 : .word 0x200006F8    (initial SP)
 * in the OEM layout. With `-mpoolsize` defaults and one literal pool
 * per function, GNU as will normally emit the pool right after the
 * function and align to 4 bytes — placement depends on linker
 * ordering. Pool placement is in scope for byte-equivalence work
 * later; for now both `ldr =` directives just pick whatever spot the
 * assembler chooses.
 */
