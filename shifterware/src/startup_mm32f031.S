/* startup_mm32f031.S
 *
 * Functional shifterware bring-up: VanMoof image header + Cortex-M0
 * vector table + Reset_Handler + Default_Handler. This file produces
 * a self-contained image that — when loaded by shifterboot at flash
 * 0x08003000 — will:
 *
 *   1. Run our Reset_Handler (slot 1 of the vector table at 0x08003028).
 *   2. Defensively reload MSP from `_estack` (slot 0 already loaded it,
 *      but shifterboot may have entered with its own SP).
 *   3. Copy `.data` initialisers from FLASH to SRAM.
 *   4. Zero `.bss`.
 *   5. Hand off to `main()` (the decomp'd OEM super-loop).
 *
 * Image layout (at flash 0x08003000):
 *
 *   0x08003000..0x08003027  (40 B)   VanMoof image header
 *   0x08003028..0x080030E7  (192 B)  Cortex-M0 vector table (48 entries)
 *   0x080030E8..           ( ... )   .text / .rodata / .data initialisers
 *
 * Differences from the OEM image:
 *
 *   - The image header CRC is left at 0; we don't yet know the
 *     algorithm shifterboot uses to validate. `imageSize` is computed
 *     at link time via `_image_end - g_vanmoof_image_header`.
 *   - The vector table entries point at OUR Reset_Handler / handlers
 *     (the OEM table is decorative, with raw addresses into OEM-only
 *     code — see `docs/memory-map.md`'s VTOR caveat). Each IRQ slot
 *     is a weak symbol aliased to Default_Handler; a strong override
 *     in another .c file replaces it.
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ---------- VanMoof image header (40 B) -------------------------------- */
    .section  .vanmoof_header, "a", %progbits
    .global   g_vanmoof_image_header
    .type     g_vanmoof_image_header, %object
g_vanmoof_image_header:
    .word   0xAA55AA55                  /* +0x00  magic                   */
    /* +0x04  version word — packed MAJOR.MINOR.PATCH.TYPE_ID (1 byte
     * each, MSB-first). Bytes: 0x00 0xED 0x02 0xC1 → MAJOR=0,
     * MINOR=237 (=0xED, matches the "0.237" in the OEM filename),
     * PATCH=2, TYPE_ID=0xC1 (shifterware). The low byte is the
     * piece shifterboot validates as the firmware-type tag; the
     * upper three are the printable version. */
    .word   0x00ED02C1
    .word   0x00000000                  /* +0x08  CRC32 — post-build patch by tools/patch_image_crc.py (MPEG-2 polynomial 0x4C11DB7) */
    .word   _image_end - g_vanmoof_image_header
                                        /* +0x0C  imageSize               */
    .ascii  "Dec 24 1985"               /* +0x10  build date (12 B w/ NUL)*/
    .byte   0
    .ascii  "00:00:00"                  /* +0x1C  build time (9 B w/ NUL) */
    .byte   0
    .byte   0xFF, 0xFF, 0xFF            /* +0x25  padding                 */
    .size   g_vanmoof_image_header, .-g_vanmoof_image_header

/* ---------- Cortex-M0 vector table (192 B) ----------------------------- */
    .section  .isr_vector, "a", %progbits
    .global   g_pfnVectors
    .type     g_pfnVectors, %object
g_pfnVectors:
    /* System vectors (16 entries) */
    .word   _estack                     /*  0: initial SP                 */
    .word   Reset_Handler               /*  1: reset                      */
    .word   NMI_Handler                 /*  2: NMI                        */
    .word   HardFault_Handler           /*  3: hard fault                 */
    .word   0                           /*  4: reserved                   */
    .word   0                           /*  5: reserved                   */
    .word   0                           /*  6: reserved                   */
    .word   0                           /*  7: reserved                   */
    .word   0                           /*  8: reserved                   */
    .word   0                           /*  9: reserved                   */
    .word   0                           /* 10: reserved                   */
    .word   SVC_Handler                 /* 11: SVCall                     */
    .word   0                           /* 12: reserved                   */
    .word   0                           /* 13: reserved                   */
    .word   PendSV_Handler              /* 14: PendSV                     */
    .word   SysTick_Handler             /* 15: SysTick                    */

    /* MM32F031 device interrupts (32 entries) */
    .word   WWDG_IRQHandler             /*  0 / 16                        */
    .word   PVD_IRQHandler              /*  1 / 17                        */
    .word   RTC_IRQHandler              /*  2 / 18                        */
    .word   FLASH_IRQHandler            /*  3 / 19                        */
    .word   RCC_IRQHandler              /*  4 / 20                        */
    .word   EXTI0_1_IRQHandler          /*  5 / 21                        */
    .word   EXTI2_3_IRQHandler          /*  6 / 22                        */
    .word   EXTI4_15_IRQHandler         /*  7 / 23                        */
    .word   HWDIV_IRQHandler            /*  8 / 24 (MM32-specific)        */
    .word   DMA1_Channel1_IRQHandler    /*  9 / 25                        */
    .word   DMA1_Channel2_3_IRQHandler  /* 10 / 26                        */
    .word   DMA1_Channel4_5_IRQHandler  /* 11 / 27                        */
    .word   ADC1_COMP_IRQHandler        /* 12 / 28                        */
    .word   TIM1_BRK_UP_TRG_COM_IRQHandler /* 13 / 29                     */
    .word   TIM1_CC_IRQHandler          /* 14 / 30                        */
    .word   TIM2_IRQHandler             /* 15 / 31                        */
    .word   TIM3_IRQHandler             /* 16 / 32                        */
    .word   TIM6_IRQHandler             /* 17 / 33                        */
    .word   0                           /* 18 / 34  reserved              */
    .word   TIM14_IRQHandler            /* 19 / 35                        */
    .word   TIM15_IRQHandler            /* 20 / 36                        */
    .word   TIM16_IRQHandler            /* 21 / 37                        */
    .word   TIM17_IRQHandler            /* 22 / 38                        */
    .word   I2C1_IRQHandler             /* 23 / 39                        */
    .word   I2C2_IRQHandler             /* 24 / 40                        */
    .word   SPI1_IRQHandler             /* 25 / 41                        */
    .word   SPI2_IRQHandler             /* 26 / 42                        */
    .word   USART1_IRQHandler           /* 27 / 43                        */
    .word   USART2_IRQHandler           /* 28 / 44                        */
    .word   0                           /* 29 / 45  reserved              */
    .word   0                           /* 30 / 46  reserved              */
    .word   0                           /* 31 / 47  reserved              */
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------- Reset_Handler ---------------------------------------------- */
    .section  .text.Reset_Handler, "ax", %progbits
    .global   Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    /* Defensive MSP reload — slot 0 already did this on hardware reset,
     * but shifterboot may have entered our image with its own SP. */
    ldr     r0, =_estack
    mov     sp, r0

    /* Copy initialised data from flash (LMA) to SRAM (VMA). */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
.Lcopy_data:
    cmp     r0, r1
    bhs     .Lcopy_data_done
    ldr     r3, [r2]
    str     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, #4
    b       .Lcopy_data
.Lcopy_data_done:

    /* Zero .bss AND the SRAM region holding the OEM's hard-coded
     * globals (G_TICK_B, the OTA staging buffer, modbus scratch, …).
     * The OEM firmware does NOT zero this region itself — it relies
     * on cold-reset SRAM being clear. When we're chain-loaded from
     * shifterboot the region may hold leftover loader state, so we
     * clear it explicitly. `_sram_clear_end` stops short of the
     * stack reservation so we don't trash the frame we're executing
     * on. */
    ldr     r0, =_sbss
    ldr     r1, =_sram_clear_end
    movs    r2, #0
.Lzero_bss:
    cmp     r0, r1
    bhs     .Lzero_bss_done
    str     r2, [r0]
    adds    r0, r0, #4
    b       .Lzero_bss
.Lzero_bss_done:

    /* Hand off to the decomp'd OEM super-loop. */
    bl      main

    /* `main()` should not return. If it does, trap. */
.Lhang:
    b       .Lhang
    .size   Reset_Handler, .-Reset_Handler

/* ---------- Default_Handler -------------------------------------------- */
    .section  .text.Default_Handler, "ax", %progbits
    .type     Default_Handler, %function
Default_Handler:
.Ldefault_loop:
    b       .Ldefault_loop
    .size   Default_Handler, .-Default_Handler

/* ---------- Weak aliases ----------------------------------------------- */
/* Each handler is a weak alias to Default_Handler. A strong definition
 * in another translation unit (e.g. `void USART1_IRQHandler(void) { ... }`
 * in src/uart.c) overrides the alias. */

    .macro  default_handler name
    .weak   \name
    .thumb_set \name, Default_Handler
    .endm

    default_handler NMI_Handler
    default_handler HardFault_Handler
    default_handler SVC_Handler
    default_handler PendSV_Handler
    default_handler SysTick_Handler

    default_handler WWDG_IRQHandler
    default_handler PVD_IRQHandler
    default_handler RTC_IRQHandler
    default_handler FLASH_IRQHandler
    default_handler RCC_IRQHandler
    default_handler EXTI0_1_IRQHandler
    default_handler EXTI2_3_IRQHandler
    default_handler EXTI4_15_IRQHandler
    default_handler HWDIV_IRQHandler
    default_handler DMA1_Channel1_IRQHandler
    default_handler DMA1_Channel2_3_IRQHandler
    default_handler DMA1_Channel4_5_IRQHandler
    default_handler ADC1_COMP_IRQHandler
    default_handler TIM1_BRK_UP_TRG_COM_IRQHandler
    default_handler TIM1_CC_IRQHandler
    default_handler TIM2_IRQHandler
    default_handler TIM3_IRQHandler
    default_handler TIM6_IRQHandler
    default_handler TIM14_IRQHandler
    default_handler TIM15_IRQHandler
    default_handler TIM16_IRQHandler
    default_handler TIM17_IRQHandler
    default_handler I2C1_IRQHandler
    default_handler I2C2_IRQHandler
    default_handler SPI1_IRQHandler
    default_handler SPI2_IRQHandler
    default_handler USART1_IRQHandler
    default_handler USART2_IRQHandler

    .end
