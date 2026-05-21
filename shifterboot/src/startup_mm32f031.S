/* startup_mm32f031.S — shifterboot
 *
 * Cortex-M0 startup for MM32F031F6U6, decompiled from
 * shifterboot.bin (SHA-512 see docs/hardware.md).
 *
 * Provenance: the OEM's Reset_Handler, vector table, and SystemInit are
 * byte-identical to MindMotion's stock pre-2021 AE-Team SDK
 * (startup_MM32F031x4x6_q.s + system_MM32F031x4x6_q.c). The only
 * bespoke pieces in the OEM are (a) the 20-byte _cold_reset stub
 * below that replaces the Keil/ARMCC __main runtime entry, (b) the
 * SysTick_Handler, and (c) the USART1_IRQHandler. See docs/hardware.md
 * for references.
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
 *      clock tree (set_sysclock_to_48m brings HCLK up to 48 MHz).
 *   3. _cold_reset @ 0x080000B4 sets MSP again, calls _init_data_bss
 *      to copy .data and zero .bss, then bx's to main.
 *   4. main @ 0x080001D8 — see src/main.c.
 *
 * Our build replicates that boot path with two simplifications:
 *   - Reset_Handler skips the SYSCFG remap-recovery probe and calls
 *     set_sysclock_to_48m directly (omitting the MindMotion SystemInit
 *     wrapper that we don't ship). HCLK still lands at 48 MHz.
 *   - _init_data_bss is a direct .data-copy + .bss-zero against the
 *     linker-exported symbols (_sdata/_edata/_sbss/_ebss/_sidata),
 *     not the OEM's table-walker pattern with ARMCC __scatterload_*
 *     helpers. Behaviour is identical (every byte of .bss zeroed,
 *     every byte of .data initialised); the wire-level scatter table
 *     at OEM `0x080017A8..0x080017C7` is not reproduced.
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
 * Vector table — placed at flash 0x08000000 by linker_mm32f031.ld via
 * the `.isr_vector` section. 48 entries (16 system + 32 IRQ),
 * matching the MM32F031F6U6 NVIC layout (which is STM32F0-style: no
 * MemManage/BusFault/UsageFault/DebugMon hardware, but the slots
 * still exist as relics of the MindMotion BSP's STM32F1-lineage
 * template — all populated with `b .` trap stubs).
 *
 * Live handlers:
 *   slot  1 (Reset)        -> Reset_Handler          (defined below)
 *   slot 15 (SysTick)      -> SysTick_Handler        (src/systick.c)
 *   slot 43 (USART1)       -> USART1_IRQHandler      (src/modbus.c)
 *
 * All other slots resolve to `Default_Handler` (`b .` — see below).
 * ------------------------------------------------------------------- */
    .section .isr_vector, "a", %progbits
    .global  __isr_vectors
    .type    __isr_vectors, %object
__isr_vectors:
    .word   _estack                 /*  0: initial Main Stack Pointer */
    .word   Reset_Handler           /*  1: Reset */
    .word   Default_Handler         /*  2: NMI */
    .word   Default_Handler         /*  3: HardFault */
    .word   Default_Handler         /*  4: MemManage (CM0: reserved, OEM stubs anyway) */
    .word   Default_Handler         /*  5: BusFault  (CM0: reserved) */
    .word   Default_Handler         /*  6: UsageFault (CM0: reserved) */
    .word   0                       /*  7: reserved */
    .word   0                       /*  8: reserved */
    .word   0                       /*  9: reserved */
    .word   0                       /* 10: reserved */
    .word   Default_Handler         /* 11: SVCall */
    .word   Default_Handler         /* 12: DebugMon (CM0: reserved) */
    .word   0                       /* 13: reserved */
    .word   Default_Handler         /* 14: PendSV */
    .word   SysTick_Handler         /* 15: SysTick — drives mdelay's countdown */

    /* External interrupts 0..31 (vector slots 16..47). */
    .word   Default_Handler         /* 16: WWDG */
    .word   Default_Handler         /* 17: PVD */
    .word   Default_Handler         /* 18: RTC */
    .word   Default_Handler         /* 19: FLASH */
    .word   Default_Handler         /* 20: RCC */
    .word   Default_Handler         /* 21: EXTI0_1 */
    .word   Default_Handler         /* 22: EXTI2_3 */
    .word   Default_Handler         /* 23: EXTI4_15 */
    .word   Default_Handler         /* 24: HSE (MM32F031 specific) */
    .word   Default_Handler         /* 25: DMA1_Channel1 */
    .word   Default_Handler         /* 26: DMA1_Channel2_3 */
    .word   Default_Handler         /* 27: DMA1_Channel4_5 */
    .word   Default_Handler         /* 28: ADC_COMP */
    .word   Default_Handler         /* 29: TIM1_BRK_UP_TRG_COM */
    .word   Default_Handler         /* 30: TIM1_CC */
    .word   0                       /* 31: reserved */
    .word   Default_Handler         /* 32: TIM3 */
    .word   0                       /* 33: reserved */
    .word   0                       /* 34: reserved */
    .word   Default_Handler         /* 35: TIM14 */
    .word   0                       /* 36: reserved */
    .word   Default_Handler         /* 37: TIM16 */
    .word   Default_Handler         /* 38: TIM17 */
    .word   Default_Handler         /* 39: I2C1 */
    .word   0                       /* 40: reserved */
    .word   Default_Handler         /* 41: SPI1 */
    .word   Default_Handler         /* 42: SPI2 */
    .word   USART1_IRQHandler       /* 43: USART1 — Modbus RX accumulator */
    .word   Default_Handler         /* 44: USART2 */
    .word   0                       /* 45: reserved */
    .word   0                       /* 46: reserved */
    .word   0                       /* 47: reserved */
    .size   __isr_vectors, .-__isr_vectors


/* ---------------------------------------------------------------------
 * Reset_Handler
 *   OEM: 0x0800060C, 46 B vendor-stock from MindMotion's
 *   startup_MM32F031x4x6_q.s. We provide a minimal equivalent here
 *   that produces the same observable end-state without depending on
 *   the vendor SystemInit:
 *     1. Set MSP from vector slot 0 (defensive: the chip does this
 *        automatically on cold reset, but warm/ROM-entry paths may
 *        skip it).
 *     2. Bring HCLK up to 48 MHz via set_sysclock_to_48m
 *        (src/clock.c; the OEM reaches the same call through
 *        SystemInit → SetSysClock).
 *     3. Branch to _cold_reset, which does .data/.bss init and
 *        finally jumps to main.
 * ------------------------------------------------------------------- */
    .section .text.Reset_Handler, "ax", %progbits
    .global  Reset_Handler
    .type    Reset_Handler, %function
    .thumb_func
Reset_Handler:
    ldr     r0, =_estack
    mov     sp, r0
    bl      set_sysclock_to_48m     /* HCLK -> 48 MHz */
    b       _cold_reset
    .size   Reset_Handler, .-Reset_Handler


/* ---------------------------------------------------------------------
 * Default_Handler
 *   OEM: FUN_08000644, 2 bytes. Single `b .` infinite loop, used by
 *   ~30 IRQ slots in the vector table. The SysTick slot (15) is
 *   NOT — that points at FUN_080014C2 (SysTick_Handler).
 *
 *   Eight more 2-byte trap stubs exist at 0x08000632..0x08000640, one
 *   per system-exception slot. They are byte-identical copies of this
 *   one; we collapse them into a single weak symbol — every vector
 *   slot that doesn't have a dedicated handler resolves here.
 *
 *   `.weak` so individual modules can override per-IRQ as more
 *   handlers get decoded (or as user code adds them).
 * ------------------------------------------------------------------- */
    .section .text.Default_Handler, "ax", %progbits
    .weak    Default_Handler
    .type    Default_Handler, %function
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
    .type    _cold_reset, %function
    .thumb_func
_cold_reset:
    ldr     r0, =_estack            /* initial MSP, mirrors vector slot 0 */
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


/* ---------------------------------------------------------------------
 * _init_data_bss
 *   OEM: FUN_08001764 @ 0x08001764, 36 B. Walks an init table at
 *   `0x080017A8..0x080017C7` (two 16-byte entries: {src, dst, len,
 *   fn}), invoking ARMCC's `__scatterload_copy` (at 0x08001788) and
 *   `__scatterload_zeroinit` (at 0x08001798) per entry. The table
 *   covers (a) 28 bytes of .data from flash `0x080017C8` to SRAM
 *   `0x20000000`, and (b) 1756 bytes of .bss at SRAM
 *   `0x2000001C..0x200006F7` (where `0x200006F8` = `_estack`).
 *
 *   Our build replaces the table-walker indirection with a direct
 *   word-by-word copy + zero against the linker-exported symbols.
 *   Behaviour is identical; byte layout differs (we don't emit the
 *   scatter table). The two scatterload helpers' bodies live in
 *   docs/progress.md as vendor-stock for later byte-match work.
 * ------------------------------------------------------------------- */
    .section .text._init_data_bss, "ax", %progbits
    .global  _init_data_bss
    .type    _init_data_bss, %function
    .thumb_func
_init_data_bss:
    /* Copy .data from flash (LMA) to SRAM (VMA). */
    ldr     r0, =_sidata            /* source: LOADADDR(.data) in flash */
    ldr     r1, =_sdata             /* dest:   _sdata in SRAM */
    ldr     r2, =_edata             /* end:    _edata in SRAM */
    b       .Ldata_cond
.Ldata_loop:
    ldm     r0!, {r3}
    stm     r1!, {r3}
.Ldata_cond:
    cmp     r1, r2
    bcc     .Ldata_loop

    /* Zero .bss. */
    movs    r3, #0
    ldr     r1, =_sbss
    ldr     r2, =_ebss
    b       .Lbss_cond
.Lbss_loop:
    stm     r1!, {r3}
.Lbss_cond:
    cmp     r1, r2
    bcc     .Lbss_loop

    bx      lr
    .size   _init_data_bss, .-_init_data_bss


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
