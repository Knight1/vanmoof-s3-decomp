/* startup_stm32f091.S
 *
 * VanMoof image header + Cortex-M0 vector table + Reset_Handler for the
 * STM32F091xC powerbankware application. Linked at 0x08008000 (the first
 * 32 KB of flash is owned by powerbankboot, which validates the header and
 * jumps here). See linker_stm32f091.ld.
 *
 * Image layout (verified from powerbank_firmware_1.11.05.bin):
 *   0x08008000  (40 B)   VanMoof image header   (.header)
 *   0x08008028  (192 B)  Cortex-M0 vector table (.vector)  SP=0x20008000, reset=0x08019270
 *
 * NOTE: this part is Cortex-M0 (no VTOR). The application relocates its
 * vector table at runtime by copying it to SRAM 0x20000000 and setting
 * SYSCFG_CFGR1.MEM_MODE = 3 — that is done inside the HAL bring-up
 * (called from main), not in this reset handler. The OEM reset itself just
 * sets SP, relocates .data, zeroes .bss, and runs SystemInit/_init/main.
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ---------- VanMoof image header (40 B) -------------------------------- */
    .section  .header, "a", %progbits
    .global   g_vanmoof_image_header
    .type     g_vanmoof_image_header, %object
g_vanmoof_image_header:
    .word   0xAA55AA55                  /* +0x00  magic                       */
    .word   0x011105B2                  /* +0x04  version 1.11.05, type 0xB2  */
    .word   0x00000000                  /* +0x08  CRC32 (post-build patch)    */
    .word   _image_end - g_vanmoof_image_header   /* +0x0C imageSize          */
    .byte   'A','p','r',' ','1','2',' ','2','0','2','1',0   /* +0x10 build date (12 B) */
    .byte   '1','0',':','1','9',':','1','4',0               /* +0x1C build time (9 B)  */
    .byte   0x00, 0x01, 0x00             /* +0x25 reserved (OEM bytes 00 01 00) */
    .size   g_vanmoof_image_header, .-g_vanmoof_image_header

/* ---------- Cortex-M0 vector table (192 B) ----------------------------- */
    .section  .vector, "a", %progbits
    .global   g_pfnVectors
    .type     g_pfnVectors, %object
g_pfnVectors:
    /* System vectors (16) */
    .word   _estack                     /*  0: initial SP (0x20008000)        */
    .word   Reset_Handler               /*  1: reset                          */
    .word   NMI_Handler                 /*  2: NMI                            */
    .word   HardFault_Handler           /*  3: hard fault (OEM = 0x0800FE2C)  */
    .word   0, 0, 0, 0, 0, 0, 0         /*  4-10: reserved                    */
    .word   SVC_Handler                 /* 11: SVCall                         */
    .word   0, 0                         /* 12-13: reserved                    */
    .word   PendSV_Handler              /* 14: PendSV                         */
    .word   SysTick_Handler             /* 15: SysTick                        */

    /* STM32F0 device interrupts (32) */
    .word   WWDG_IRQHandler             /*  0 */
    .word   PVD_VDDIO2_IRQHandler       /*  1 */
    .word   RTC_IRQHandler              /*  2 */
    .word   FLASH_IRQHandler            /*  3 */
    .word   RCC_CRS_IRQHandler          /*  4 */
    .word   EXTI0_1_IRQHandler          /*  5 */
    .word   EXTI2_3_IRQHandler          /*  6 */
    .word   EXTI4_15_IRQHandler         /*  7 */
    .word   TSC_IRQHandler              /*  8 */
    .word   DMA1_Ch1_IRQHandler         /*  9 */
    .word   DMA1_Ch2_3_DMA2_Ch1_2_IRQHandler   /* 10 */
    .word   DMA1_Ch4_7_DMA2_Ch3_5_IRQHandler   /* 11 */
    .word   ADC1_COMP_IRQHandler        /* 12 */
    .word   TIM1_BRK_UP_TRG_COM_IRQHandler /* 13 */
    .word   TIM1_CC_IRQHandler          /* 14 */
    .word   TIM2_IRQHandler             /* 15 */
    .word   TIM3_IRQHandler             /* 16 */
    .word   TIM6_DAC_IRQHandler         /* 17 */
    .word   TIM7_IRQHandler             /* 18 */
    .word   TIM14_IRQHandler            /* 19 */
    .word   TIM15_IRQHandler            /* 20 */
    .word   TIM16_IRQHandler            /* 21 */
    .word   TIM17_IRQHandler            /* 22 */
    .word   I2C1_IRQHandler             /* 23 */
    .word   I2C2_IRQHandler             /* 24 */
    .word   SPI1_IRQHandler             /* 25 */
    .word   SPI2_IRQHandler             /* 26 */
    .word   USART1_IRQHandler           /* 27 */
    .word   USART2_IRQHandler           /* 28 */
    .word   USART3_8_IRQHandler         /* 29 */
    .word   CEC_CAN_IRQHandler          /* 30 */
    .word   0                           /* 31: reserved                       */
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------- Reset_Handler ---------------------------------------------- */
    .section  .text.Reset_Handler, "ax", %progbits
    .global   Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    ldr     r0, =_estack
    mov     sp, r0

    /* Copy .data initialisers from flash to SRAM */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
    movs    r3, #0
    b       .Lcopy_cond
.Lcopy_body:
    ldr     r4, [r2, r3]
    str     r4, [r0, r3]
    adds    r3, r3, #4
.Lcopy_cond:
    adds    r4, r0, r3
    cmp     r4, r1
    bcc     .Lcopy_body

    /* Zero .bss */
    ldr     r2, =_sbss
    ldr     r4, =_ebss
    movs    r3, #0
    b       .Lzero_cond
.Lzero_body:
    str     r3, [r2]
    adds    r2, r2, #4
.Lzero_cond:
    cmp     r2, r4
    bcc     .Lzero_body

    bl      SystemInit
    bl      __libc_init_array_lite
    bl      main
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
    default_handler PVD_VDDIO2_IRQHandler
    default_handler RTC_IRQHandler
    default_handler FLASH_IRQHandler
    default_handler RCC_CRS_IRQHandler
    default_handler EXTI0_1_IRQHandler
    default_handler EXTI2_3_IRQHandler
    default_handler EXTI4_15_IRQHandler
    default_handler TSC_IRQHandler
    default_handler DMA1_Ch1_IRQHandler
    default_handler DMA1_Ch2_3_DMA2_Ch1_2_IRQHandler
    default_handler DMA1_Ch4_7_DMA2_Ch3_5_IRQHandler
    default_handler ADC1_COMP_IRQHandler
    default_handler TIM1_BRK_UP_TRG_COM_IRQHandler
    default_handler TIM1_CC_IRQHandler
    default_handler TIM2_IRQHandler
    default_handler TIM3_IRQHandler
    default_handler TIM6_DAC_IRQHandler
    default_handler TIM7_IRQHandler
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
    default_handler USART3_8_IRQHandler
    default_handler CEC_CAN_IRQHandler

    .end
