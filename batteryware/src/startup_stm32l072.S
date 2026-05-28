/* startup_stm32l072.S
 *
 * VanMoof image header + Cortex-M0+ vector table + Reset_Handler
 * for STM32L072CZT6. Produces a self-contained firmware image
 * that initialises .data, zeroes .bss, and calls main() via the
 * standard CMSIS startup chain.
 *
 * The application is linked at 0x08005000 — the first 20 KB of flash
 * is owned by `bmsboot`, which jumps here after verifying the image
 * header. See linker_stm32l072.ld.
 *
 * Image layout:
 *   0x08005000  (40 B)   VanMoof image header   (.header)
 *   0x08005028  (192 B)  Cortex-M0+ vector table (.vector)
 *   0x080050E8  ( ... )  .text / .rodata / .data LMA
 */

    .syntax unified
    .cpu    cortex-m0plus
    .thumb

/* ---------- VanMoof image header (40 B) -------------------------------- */
    .section  .header, "a", %progbits
    .global   g_vanmoof_image_header
    .type     g_vanmoof_image_header, %object
g_vanmoof_image_header:
    .word   0xAA55AA55                  /* +0x00  magic                   */
    .word   0x011701B1                  /* +0x04  version (1.17.1 BMS)    */
    .word   0x00000000                  /* +0x08  CRC32 (post-build patch) */
    .word   _image_end - g_vanmoof_image_header
                                        /* +0x0C  imageSize               */
    .space  0x15, 0                     /* +0x10  build date/time: OEM    */
                                        /*        zeroes 0x10..0x24 (21 B) */
    .byte   0xFF, 0xFF, 0xFF            /* +0x25  padding                 */
    .size   g_vanmoof_image_header, .-g_vanmoof_image_header

/* ---------- Cortex-M0+ vector table (192 B) ---------------------------- */
    .section  .vector, "a", %progbits
    .global   g_pfnVectors
    .type     g_pfnVectors, %object
g_pfnVectors:
    /* System vectors (16 entries) */
    .word   _estack                     /*  0: initial SP                 */
    .word   Reset_Handler               /*  1: reset                      */
    .word   NMI_Handler                 /*  2: NMI                        */
    .word   HardFault_Handler           /*  3: hard fault                 */
    .word   0                           /*  4: reserved (MemManage)       */
    .word   0                           /*  5: reserved (BusFault)        */
    .word   0                           /*  6: reserved (UsageFault)      */
    .word   0                           /*  7: reserved                   */
    .word   0                           /*  8: reserved                   */
    .word   0                           /*  9: reserved                   */
    .word   0                           /* 10: reserved                   */
    .word   SVC_Handler                 /* 11: SVCall                     */
    .word   0                           /* 12: reserved (DebugMon)        */
    .word   0                           /* 13: reserved                   */
    .word   PendSV_Handler              /* 14: PendSV                     */
    .word   SysTick_Handler             /* 15: SysTick                    */

    /* STM32L0 device interrupts (32 entries) */
    .word   WWDG_IRQHandler             /*  0 / 16                        */
    .word   PVD_IRQHandler              /*  1 / 17                        */
    .word   RTC_IRQHandler              /*  2 / 18                        */
    .word   FLASH_IRQHandler            /*  3 / 19                        */
    .word   RCC_IRQHandler              /*  4 / 20                        */
    .word   EXTI0_1_IRQHandler          /*  5 / 21                        */
    .word   EXTI2_3_IRQHandler          /*  6 / 22                        */
    .word   EXTI4_15_IRQHandler         /*  7 / 23                        */
    .word   0                           /*  8 / 24  reserved              */
    .word   DMA1_Channel1_IRQHandler    /*  9 / 25                        */
    .word   DMA1_Channel2_3_IRQHandler  /* 10 / 26                        */
    .word   DMA1_Channel4_5_6_7_IRQHandler /* 11 / 27                     */
    .word   ADC1_COMP_IRQHandler        /* 12 / 28                        */
    .word   LPTIM1_IRQHandler           /* 13 / 29                        */
    .word   0                           /* 14 / 30  reserved              */
    .word   TIM2_IRQHandler             /* 15 / 31                        */
    .word   0                           /* 16 / 32  reserved              */
    .word   TIM6_DAC_IRQHandler         /* 17 / 33                        */
    .word   0                           /* 18 / 34  reserved              */
    .word   0                           /* 19 / 35  reserved              */
    .word   TIM21_IRQHandler            /* 20 / 36                        */
    .word   0                           /* 21 / 37  reserved              */
    .word   TIM22_IRQHandler            /* 22 / 38                        */
    .word   I2C1_IRQHandler             /* 23 / 39                        */
    .word   I2C2_IRQHandler             /* 24 / 40                        */
    .word   SPI1_IRQHandler             /* 25 / 41                        */
    .word   SPI2_IRQHandler             /* 26 / 42                        */
    .word   USART1_IRQHandler           /* 27 / 43                        */
    .word   USART2_IRQHandler           /* 28 / 44                        */
    .word   LPUART1_AES_RNG_IRQHandler  /* 29 / 45                        */
    .word   LCD_IRQHandler              /* 30 / 46                        */
    .word   USB_IRQHandler              /* 31 / 47                        */
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------- Reset_Handler ---------------------------------------------- */
    .section  .text.Reset_Handler, "ax", %progbits
    .global   Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    /* OEM disables interrupts on entry (file 0x0E1F8: b672 cpsid i) */
    cpsid   i

    /* Reload stack pointer from linker-defined _estack */
    ldr     r0, =_estack
    mov     sp, r0

    /* Copy .data initialisers from flash to SRAM */
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

    /* Zero .bss */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
.Lzero_bss:
    cmp     r0, r1
    bhs     .Lzero_bss_done
    str     r2, [r0]
    adds    r0, r0, #4
    b       .Lzero_bss
.Lzero_bss_done:

    /* CMSIS pre-main hooks, then main. Matches OEM Reset_Handler
     * (runtime addresses; file offsets in our .bin in parens):
     *   bl SystemInit                  0x08016E14  (file 0x11E14)
     *   bl __libc_init_array_lite      0x08016E20  (file 0x11E20)
     *   bl main                        0x0800A7B0  (file 0x057B0)
     */
    bl      SystemInit
    bl      __libc_init_array_lite
    bl      main

    /* Should not return — trap if it does */
.Lhang:
    b       .Lhang
    .size   Reset_Handler, .-Reset_Handler

/* ---------- SystemInit (runtime 0x08016E14, file 0x11E14) -------------- */
/* OEM body is an empty -O0 frame: push/add r7/nop/mov sp/pop. */
    .section  .text.SystemInit, "ax", %progbits
    .global   SystemInit
    .type     SystemInit, %function
SystemInit:
    push    {r7, lr}
    add     r7, sp, #0
    nop
    mov     sp, r7
    pop     {r7, pc}
    .size   SystemInit, .-SystemInit

/* ---------- __libc_init_array_lite (runtime 0x08016E20, file 0x11E20) -- */
/* Standard newlib __libc_init_array: run .preinit_array, call _init,
 * run .init_array. In the OEM all four boundary symbols resolve to the
 * same address (empty arrays), so this reduces to a single _init call;
 * the loop bodies are preserved verbatim. The .preinit_array /
 * .init_array sections in the linker script keep the symbols defined
 * (and empty, matching the OEM). */
    .section  .text.__libc_init_array_lite, "ax", %progbits
    .global   __libc_init_array_lite
    .type     __libc_init_array_lite, %function
__libc_init_array_lite:
    push    {r4, r5, r6, lr}
    movs    r6, #0
    ldr     r5, =__preinit_array_start
    ldr     r4, =__preinit_array_end
    subs    r4, r4, r5
    asrs    r4, r4, #2
.Lpre_cond:
    cmp     r6, r4
    bne     .Lpre_body
    movs    r6, #0
    bl      _init
    ldr     r5, =__init_array_start
    ldr     r4, =__init_array_end
    subs    r4, r4, r5
    asrs    r4, r4, #2
.Linit_cond:
    cmp     r6, r4
    bne     .Linit_body
    pop     {r4, r5, r6, pc}
.Lpre_body:
    lsls    r3, r6, #2
    ldr     r3, [r5, r3]
    blx     r3
    adds    r6, #1
    b       .Lpre_cond
.Linit_body:
    lsls    r3, r6, #2
    ldr     r3, [r5, r3]
    blx     r3
    adds    r6, #1
    b       .Linit_cond
    .pool
    .size   __libc_init_array_lite, .-__libc_init_array_lite

/* ---------- _init (runtime 0x08016E8C, file 0x11E8C) ------------------- */
/* crti.o/crtn.o-style _init with nothing in between: save/restore the
 * call-clobbered set and return. Functionally a no-op (no C constructors
 * in this image), preserved for byte-faithfulness. */
    .section  .text._init, "ax", %progbits
    .global   _init
    .type     _init, %function
_init:
    push    {r3, r4, r5, r6, r7, lr}
    nop
    pop     {r3, r4, r5, r6, r7}
    pop     {r3}
    mov     lr, r3
    bx      lr
    .size   _init, .-_init

/* ---------- Default_Handler -------------------------------------------- */
    .section  .text.Default_Handler, "ax", %progbits
    .type     Default_Handler, %function
Default_Handler:
.Ldefault_loop:
    b       .Ldefault_loop
    .size   Default_Handler, .-Default_Handler

/* ---------- Weak aliases ----------------------------------------------- */
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
    default_handler DMA1_Channel1_IRQHandler
    default_handler DMA1_Channel2_3_IRQHandler
    default_handler DMA1_Channel4_5_6_7_IRQHandler
    default_handler ADC1_COMP_IRQHandler
    default_handler LPTIM1_IRQHandler
    default_handler TIM2_IRQHandler
    default_handler TIM6_DAC_IRQHandler
    default_handler TIM21_IRQHandler
    default_handler TIM22_IRQHandler
    default_handler I2C1_IRQHandler
    default_handler I2C2_IRQHandler
    default_handler SPI1_IRQHandler
    default_handler SPI2_IRQHandler
    default_handler USART1_IRQHandler
    default_handler USART2_IRQHandler
    default_handler LPUART1_AES_RNG_IRQHandler
    default_handler LCD_IRQHandler
    default_handler USB_IRQHandler

    .end
