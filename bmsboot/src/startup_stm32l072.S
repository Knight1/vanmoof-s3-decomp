/* startup_stm32l072.S
 *
 * Cortex-M0+ vector table + Reset_Handler for the STM32L072CZT6 bmsboot loader.
 * Linked at 0x08000000 — the bootloader owns the base of flash and the CPU
 * resets straight into this table. There is NO VanMoof image header on the
 * loader (that header only prefixes the AP / Shadow application banks).
 *
 * Verified from bmsboot_v007.bin:
 *   vec[0]  SP        = 0x20005000   (top of 20 KB SRAM)
 *   vec[1]  Reset     = 0x08001E60
 *   vec[2]  NMI       = 0x08001EB0   (Default_Handler)
 *   vec[3]  HardFault = 0x08000C50
 *   vec[11] SVCall    = 0x08001EB0   (Default_Handler)
 *   vec[14] PendSV    = 0x08001EB0   (Default_Handler)
 *   vec[15] SysTick   = 0x0800169C
 *   IRQ27 USART1      = 0x080017E8   (comms / OTA RX-TX ISR)
 *   all other IRQs    = 0x08001EB0   (Default_Handler, "b .")
 *
 * The OEM Reset_Handler is a minimal stub: set SP, copy .data + zero .bss
 * inline (no separate init_data_bss routine), then SystemInit (empty),
 * __libc_init_array and main. main() never returns.
 */

    .syntax unified
    .cpu    cortex-m0plus
    .thumb

/* ---------- Cortex-M0+ vector table (48 words, at flash base) ---------- */
    .section  .isr_vector, "a", %progbits
    .global   g_pfnVectors
    .type     g_pfnVectors, %object
g_pfnVectors:
    /* System vectors (16) */
    .word   _estack                     /*  0: initial SP (0x20005000)        */
    .word   Reset_Handler               /*  1: reset                          */
    .word   NMI_Handler                 /*  2: NMI  -> Default                 */
    .word   HardFault_Handler           /*  3: hard fault                     */
    .word   0, 0, 0, 0, 0, 0, 0         /*  4-10: reserved                    */
    .word   SVC_Handler                 /* 11: SVCall -> Default               */
    .word   0, 0                         /* 12-13: reserved                    */
    .word   PendSV_Handler              /* 14: PendSV -> Default               */
    .word   SysTick_Handler             /* 15: SysTick                        */

    /* STM32L0 device interrupts (32) */
    .word   WWDG_IRQHandler             /*  0 */
    .word   PVD_IRQHandler              /*  1 */
    .word   RTC_IRQHandler              /*  2 */
    .word   FLASH_IRQHandler            /*  3 */
    .word   RCC_IRQHandler              /*  4 */
    .word   EXTI0_1_IRQHandler          /*  5 */
    .word   EXTI2_3_IRQHandler          /*  6 */
    .word   EXTI4_15_IRQHandler         /*  7 */
    .word   Default_Handler             /*  8: reserved (OEM = Default)        */
    .word   DMA1_Channel1_IRQHandler    /*  9 */
    .word   DMA1_Channel2_3_IRQHandler  /* 10 */
    .word   DMA1_Channel4_7_IRQHandler  /* 11 */
    .word   ADC1_COMP_IRQHandler        /* 12 */
    .word   LPTIM1_IRQHandler           /* 13 */
    .word   Default_Handler             /* 14: reserved (OEM = Default)        */
    .word   TIM2_IRQHandler             /* 15 */
    .word   Default_Handler             /* 16: reserved (OEM = Default)        */
    .word   TIM6_DAC_IRQHandler         /* 17 */
    .word   Default_Handler             /* 18: reserved (OEM = Default)        */
    .word   0                           /* 19: unused (OEM = 0)                */
    .word   TIM21_IRQHandler            /* 20 */
    .word   Default_Handler             /* 21: reserved (OEM = Default)        */
    .word   TIM22_IRQHandler            /* 22 */
    .word   I2C1_IRQHandler             /* 23 */
    .word   I2C2_IRQHandler             /* 24 */
    .word   SPI1_IRQHandler             /* 25 */
    .word   SPI2_IRQHandler             /* 26 */
    .word   USART1_IRQHandler           /* 27: comms / OTA RX-TX ISR          */
    .word   USART2_IRQHandler           /* 28 */
    .word   LPUART1_IRQHandler          /* 29 */
    .word   0                           /* 30: unused (OEM = 0)                */
    .word   USB_IRQHandler              /* 31 */
    .size   g_pfnVectors, .-g_pfnVectors

/* ---------- Reset_Handler (OEM 0x08001E60) ----------------------------- *
 * Inline .data copy + .bss zero (index-addressed, matching the OEM form),
 * then SystemInit / __libc_init_array / main. No SystemInit clock work — the
 * clock tree is brought up later inside boot_hw_init() (called from main).  */
    .section  .text.Reset_Handler, "ax", %progbits
    .global   Reset_Handler
    .type     Reset_Handler, %function
Reset_Handler:
    ldr     r0, =_estack
    mov     sp, r0

    /* copy .data: for (r3=0; _sdata+r3 < _edata; r3+=4) [_sdata+r3]=[_sidata+r3] */
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

    /* zero .bss: for (r2=_sbss; r2 < _ebss; r2+=4) *r2 = 0 */
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
    bl      __libc_init_array
    bl      main
.Lhang:
    b       .Lhang
    .size   Reset_Handler, .-Reset_Handler

/* ---------- SystemInit (OEM 0x08004860) -------------------------------- */
/* Empty -O0 frame in the OEM (clocks come up later in boot_hw_init). */
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

/* ---------- __libc_init_array (OEM 0x0800486C) ------------------------- */
/* Standard newlib startup: run .preinit_array, call _init, run .init_array.
 * Both arrays are empty in this image (start == end), so it reduces to a
 * single _init call; the loop bodies are preserved verbatim. */
    .section  .text.__libc_init_array, "ax", %progbits
    .global   __libc_init_array
    .type     __libc_init_array, %function
__libc_init_array:
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
    .size   __libc_init_array, .-__libc_init_array

/* ---------- _init (OEM 0x080048D8) ------------------------------------- */
/* crti/crtn-style _init with nothing in between (no C constructors). */
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

/* ---------- Default_Handler (OEM 0x08001EB0) --------------------------- */
    .section  .text.Default_Handler, "ax", %progbits
    .global   Default_Handler
    .type     Default_Handler, %function
Default_Handler:
.Ldefault_loop:
    b       .Ldefault_loop
    .size   Default_Handler, .-Default_Handler

/* Weak fallbacks: HardFault, SysTick and USART1 are implemented in C and
 * override these; everything else (incl. NMI/SVC/PendSV, per the OEM table)
 * stays trapped at Default_Handler. */
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
    default_handler DMA1_Channel4_7_IRQHandler
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
    default_handler LPUART1_IRQHandler
    default_handler LCD_IRQHandler
    default_handler USB_IRQHandler

    .end
