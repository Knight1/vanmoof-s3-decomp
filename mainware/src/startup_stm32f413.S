/* mainware startup — VanMoof container envelope, STM32F413 vector table, and
 * Reset_Handler. Reproduces the OEM boot path (Reset_Handler 0x08043E54):
 * set SP, copy .data, zero .bss, paint free RAM with 0x0000000E, then
 * SystemInit -> __libc_init_array -> main.
 *
 * This roots the call graph so the build links to a real (non-empty) image.
 * The image is NOT byte-identical to the OEM: our decoded functions land at
 * linker-assigned addresses, not their OEM addresses, so the vector table and
 * the .text bodies differ. Byte-for-byte comparison would require pinning every
 * function to its OEM address (a matching-decomp setup) — see docs/progress.md.
 */

    .syntax unified
    .cpu cortex-m4
    .fpu softvfp
    .thumb

    .global g_pfnVectors
    .global Default_Handler

/* ---------------------------------------------------------------------------
 * VanMoof container envelope (512 B at the slot base, before the vector table).
 * Magic + version + build-date are reproduced exactly; the CRC32 and total
 * length are post-build patches (zeroed here) — see docs/hardware.md.
 * ------------------------------------------------------------------------- */
    .section .envelope,"a",%progbits
    .byte   0x55, 0xAA, 0x55, 0xAA  /* +0x000 magic (file byte order) */
    .byte   0xF4                    /* +0x004 flag (constant across versions) */
    .byte   0x06                    /* +0x005 patch (1.07.06) */
    .byte   0x07                    /* +0x006 minor */
    .byte   0x01                    /* +0x007 major */
    .word   0x00000000              /* +0x008 CRC32 (VanMoof poly) — patched post-build */
    .word   0x00000000              /* +0x00C total image length — patched post-build */
    .asciz  "Nov  1 2021"           /* +0x010 __DATE__ (12 B incl NUL) */
    .asciz  "10:25:04"              /* +0x01C __TIME__ (9 B incl NUL) */
    .space  0x200 - 0x025, 0xFF     /* +0x025..+0x1FF: 0xFF padding to VTOR */

/* ---------------------------------------------------------------------------
 * Vector table (flash 0x08020200). System slots reference our decoded handlers;
 * the 112 IRQ slots route to Default_Handler until decoded.
 * ------------------------------------------------------------------------- */
    .section .isr_vector,"a",%progbits
    .type   g_pfnVectors, %object
g_pfnVectors:
    .word   _estack                 /* 0  initial SP */
    .word   Reset_Handler           /* 1  */
    .word   NMI_Handler             /* 2  */
    .word   HardFault_Handler       /* 3  */
    .word   MemManage_Handler       /* 4  */
    .word   BusFault_Handler        /* 5  */
    .word   UsageFault_Handler      /* 6  */
    .word   0                       /* 7  reserved */
    .word   0                       /* 8  reserved */
    .word   0                       /* 9  reserved */
    .word   0                       /* 10 reserved */
    .word   SVC_Handler             /* 11 */
    .word   DebugMon_Handler        /* 12 */
    .word   0                       /* 13 reserved */
    .word   PendSV_Handler          /* 14 */
    .word   SysTick_Handler         /* 15 */
    .rept   112                     /* 16..127 external IRQs (undecoded) */
    .word   Default_Handler
    .endr
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------------------------------------------------------------------------
 * Reset_Handler (OEM 0x08043E54).
 * ------------------------------------------------------------------------- */
    .section .text.Reset_Handler,"ax",%progbits
    .weak   Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:
    ldr     sp, =_estack

    movs    r1, #0
    b       LoopCopyData
CopyData:
    ldr     r3, =_sidata
    ldr     r3, [r3, r1]
    str     r3, [r0, r1]
    adds    r1, r1, #4
LoopCopyData:
    ldr     r0, =_sdata
    ldr     r3, =_edata
    adds    r2, r0, r1
    cmp     r2, r3
    bcc     CopyData

    ldr     r2, =_sbss
    b       LoopZeroBss
ZeroBss:
    movs    r3, #0
    str     r3, [r2], #4
LoopZeroBss:
    ldr     r3, =_ebss
    cmp     r2, r3
    bcc     ZeroBss

    /* Paint [_ebss, _estack) with 0x0000000E — the OEM's free-RAM/stack fill. */
    ldr     r2, =_ebss
    ldr     r4, =_estack
    movs    r3, #0x0E
    b       LoopFill
Fill:
    str     r3, [r2], #4
LoopFill:
    cmp     r2, r4
    bcc     Fill

    bl      SystemInit
    bl      __libc_init_array
    bl      main
    bx      lr
    .size   Reset_Handler, .-Reset_Handler

/* ---------------------------------------------------------------------------
 * Default_Handler — spin (the OEM's undecoded IRQ slots; also the link target
 * for system slots not yet given a real body).
 * ------------------------------------------------------------------------- */
    .section .text.Default_Handler,"ax",%progbits
    .type   Default_Handler, %function
Default_Handler:
    b       .
    .size   Default_Handler, .-Default_Handler

/* ---------------------------------------------------------------------------
 * Link entry + placeholders so the partial tree links:
 *   main             — the real super-loop is sourced in src/main.c (OEM
 *                      0x0803DEA8) but defined __attribute__((weak)) there, so
 *                      this STRONG spin-stub remains the link target while ~70
 *                      of main's callees are still unsourced (the closure isn't
 *                      ready to root). When it is, drop this stub and flip
 *                      src/main.c's main to strong so `bl main` runs the loop.
 *   __libc_init_array— newlib CRT (vendor-stock); empty until libc is linked.
 * ------------------------------------------------------------------------- */
    .section .text.main_stub,"ax",%progbits
    .global main
    .type   main, %function
main:
    b       .
    .size   main, .-main

    .section .text.libc_init_stub,"ax",%progbits
    .weak   __libc_init_array
    .type   __libc_init_array, %function
__libc_init_array:
    bx      lr
    .size   __libc_init_array, .-__libc_init_array
