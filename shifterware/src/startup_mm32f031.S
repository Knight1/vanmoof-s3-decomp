/* startup_mm32f031.S
 *
 * Cortex-M0 vector table + reset stub for MM32F031F6U6.
 *
 * Vector layout: 16 system vectors + 32 device IRQs = 48 entries (192
 * bytes). Names follow STM32F031/CMSIS convention; the MM32F031 IRQ map
 * matches closely. Each handler is WEAK and aliased to Default_Handler so
 * unimplemented vectors trap into a known infinite loop.
 *
 * Reset_Handler does the C runtime startup that is universally required:
 *   1. set MSP to _estack (already done by hardware from vector[0], but
 *      we redo it defensively).
 *   2. copy .data initialisers from FLASH to SRAM.
 *   3. zero .bss.
 *   4. call SystemInit() — clock tree, flash latency, peripheral reset.
 *   5. call main(). If main returns, spin.
 *
 * Replace any specific handler in another .c file with a non-weak symbol
 * of the same name, e.g.  void TIM3_IRQHandler(void) { ... }
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ---------- Vector table ----------------------------------------------- */
    .section  .isr_vector, "a", %progbits
    .type     g_pfnVectors, %object
    .size     g_pfnVectors, .-g_pfnVectors

g_pfnVectors:
    /* Cortex-M0 system vectors */
    .word   _estack                     /*  0: initial SP             */
    .word   Reset_Handler               /*  1: reset                  */
    .word   NMI_Handler                 /*  2: NMI                    */
    .word   HardFault_Handler           /*  3: hard fault             */
    .word   0                           /*  4: reserved               */
    .word   0                           /*  5: reserved               */
    .word   0                           /*  6: reserved               */
    .word   0                           /*  7: reserved               */
    .word   0                           /*  8: reserved               */
    .word   0                           /*  9: reserved               */
    .word   0                           /* 10: reserved               */
    .word   SVC_Handler                 /* 11: SVCall                 */
    .word   0                           /* 12: reserved               */
    .word   0                           /* 13: reserved               */
    .word   PendSV_Handler              /* 14: PendSV                 */
    .word   SysTick_Handler             /* 15: SysTick                */

    /* MM32F031 device interrupts (matches STM32F031 layout) */
    .word   WWDG_IRQHandler             /*  0 / 16                    */
    .word   PVD_IRQHandler              /*  1 / 17                    */
    .word   RTC_IRQHandler              /*  2 / 18                    */
    .word   FLASH_IRQHandler            /*  3 / 19                    */
    .word   RCC_IRQHandler              /*  4 / 20                    */
    .word   EXTI0_1_IRQHandler          /*  5 / 21                    */
    .word   EXTI2_3_IRQHandler          /*  6 / 22                    */
    .word   EXTI4_15_IRQHandler         /*  7 / 23                    */
    .word   HWDIV_IRQHandler            /*  8 / 24  (MM32 specific)   */
    .word   DMA1_Channel1_IRQHandler    /*  9 / 25                    */
    .word   DMA1_Channel2_3_IRQHandler  /* 10 / 26                    */
    .word   DMA1_Channel4_5_IRQHandler  /* 11 / 27                    */
    .word   ADC1_COMP_IRQHandler        /* 12 / 28                    */
    .word   TIM1_BRK_UP_TRG_COM_IRQHandler /* 13 / 29                 */
    .word   TIM1_CC_IRQHandler          /* 14 / 30                    */
    .word   TIM2_IRQHandler             /* 15 / 31                    */
    .word   TIM3_IRQHandler             /* 16 / 32                    */
    .word   TIM6_IRQHandler             /* 17 / 33                    */
    .word   0                           /* 18 / 34  reserved          */
    .word   TIM14_IRQHandler            /* 19 / 35                    */
    .word   TIM15_IRQHandler            /* 20 / 36                    */
    .word   TIM16_IRQHandler            /* 21 / 37                    */
    .word   TIM17_IRQHandler            /* 22 / 38                    */
    .word   I2C1_IRQHandler             /* 23 / 39                    */
    .word   I2C2_IRQHandler             /* 24 / 40                    */
    .word   SPI1_IRQHandler             /* 25 / 41                    */
    .word   SPI2_IRQHandler             /* 26 / 42                    */
    .word   USART1_IRQHandler           /* 27 / 43                    */
    .word   USART2_IRQHandler           /* 28 / 44                    */
    .word   0                           /* 29 / 45  reserved          */
    .word   0                           /* 30 / 46  reserved (CEC)    */
    .word   0                           /* 31 / 47  reserved (USB)    */

/* ---------- Reset handler ---------------------------------------------- */
    .section  .text.Reset_Handler
    .weak     Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    /* Defensive MSP load (hardware already did this from vector[0]). */
    ldr     r0, =_estack
    mov     sp, r0

    /* Copy .data from FLASH (_sidata) to SRAM (_sdata.._edata). */
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

    /* Zero .bss. */
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

    /* Low-level system init (clocks, flash latency). */
    bl      SystemInit

    /* Application entry. */
    bl      main

    /* main() returned — spin. The OEM probably issues a system reset
     * here; replace once observed in Ghidra. */
.Lhang:
    b       .Lhang
    .size Reset_Handler, .-Reset_Handler

/* ---------- Default handler ------------------------------------------- */
/* Anything not yet implemented lands here. Set a breakpoint in your
 * debugger on Default_Handler to catch unexpected vectors. */
    .section  .text.Default_Handler, "ax", %progbits
    .type     Default_Handler, %function
Default_Handler:
.Ldefault_loop:
    b       .Ldefault_loop
    .size Default_Handler, .-Default_Handler

/* ---------- Weak aliases ---------------------------------------------- */
/* Each .weak  + .thumb_set pair lets a handler in another translation
 * unit override the default by simply defining a function with the same
 * name. */

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
