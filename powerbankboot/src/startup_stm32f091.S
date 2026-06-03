/* startup_stm32f091.S
 *
 * Cortex-M0 vector table + Reset_Handler for the STM32F091xC powerbankboot
 * loader. Linked at 0x08000000 — the bootloader owns the base of flash and the
 * CPU resets straight into this table. There is NO VanMoof image header on the
 * loader (that header only prefixes the AP / Shadow application banks).
 *
 * Verified from powerbank_bootloader_1.00.bin:
 *   vec[0]  SP        = 0x20008000   (top of 32 KB SRAM)
 *   vec[1]  Reset     = 0x08002878
 *   vec[2]  NMI       = 0x080016AC   (clock-security / CSS handler)
 *   vec[3]  HardFault = 0x080016FC
 *   vec[11] SVCall    = 0x08001718
 *   vec[14] PendSV    = 0x08001734
 *   vec[15] SysTick   = 0x0800214C
 *   IRQ17 TIM6_DAC    = 0x08001158   (STL clock-measurement input capture)
 *   IRQ28 USART2      = 0x08002248   (comms RX/TX ISR)
 *   all other IRQs    = 0x0800289C   (Default_Handler, "b .")
 *
 * The OEM Reset_Handler is a minimal stub: set SP, run the .data/.bss init
 * (a standalone routine the STL re-invokes between phases), then branch to
 * main. It does NOT call SystemInit — the clock tree is brought up later, in
 * boot_hw_init() (called from boot_main).
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ---------- Cortex-M0 vector table (48 words, at flash base) ----------- */
    .section  .isr_vector, "a", %progbits
    .global   g_pfnVectors
    .type     g_pfnVectors, %object
g_pfnVectors:
    /* System vectors (16) */
    .word   _estack                     /*  0: initial SP (0x20008000)        */
    .word   Reset_Handler               /*  1: reset                          */
    .word   NMI_Handler                 /*  2: NMI  (CSS clock-security)       */
    .word   HardFault_Handler           /*  3: hard fault                     */
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
    .word   TIM6_DAC_IRQHandler         /* 17: STL clock-measurement capture  */
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
    .word   USART2_IRQHandler           /* 28: comms RX/TX ISR                */
    .word   USART3_8_IRQHandler         /* 29 */
    .word   CEC_CAN_IRQHandler          /* 30 */
    .word   0                           /* 31: reserved                       */
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------- Reset_Handler (OEM 0x08002878) ----------------------------- */
    .section  .text.Reset_Handler, "ax", %progbits
    .global   Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    ldr     r0, =_estack
    mov     sp, r0
    bl      init_data_bss
    ldr     r0, =main
    bx      r0
    .size   Reset_Handler, .-Reset_Handler

/* ---------- init_data_bss (OEM 0x0800283C) ----------------------------- *
 * Standalone .data copy + .bss zero. The STL main re-invokes this between
 * phases to refresh the RAM image, so it is a reusable routine (not inlined
 * into reset) and must remain idempotent.                                  */
    .section  .text.init_data_bss, "ax", %progbits
    .global   init_data_bss
    .type     init_data_bss, %function
init_data_bss:
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
    bx      lr
    .size   init_data_bss, .-init_data_bss

/* ---------- Default_Handler (OEM 0x0800289C) --------------------------- */
    .section  .text.Default_Handler, "ax", %progbits
    .global   Default_Handler
    .type     Default_Handler, %function
Default_Handler:
.Ldefault_loop:
    b       .Ldefault_loop
    .size   Default_Handler, .-Default_Handler

/* Weak fallbacks: the seven live handlers below are implemented in C and
 * override these; everything else stays trapped at Default_Handler. */
    .macro  default_handler name
    .weak   \name
    .thumb_set \name, Default_Handler
    .endm

    default_handler NMI_Handler
    default_handler HardFault_Handler
    default_handler SVC_Handler
    default_handler PendSV_Handler
    default_handler SysTick_Handler
    default_handler TIM6_DAC_IRQHandler
    default_handler USART2_IRQHandler

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
    default_handler USART3_8_IRQHandler
    default_handler CEC_CAN_IRQHandler

    .end
