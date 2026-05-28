#include "batteryware.h"

/*
 * Batteryware main entry point — early boot setup.
 *
 * Faithful translation of OEM FUN_080072A8 (156 B). Sequence:
 *
 *   1. Enable NVIC IRQ 27 (with DSB).
 *   2. RCC->APB2ENR (0x40021034) &= ~bit14   — disable USART1 clock.
 *   3. EXTI->PR (0x40010414) = 1             — clear pending EXTI line 0.
 *   4. Enable NVIC IRQ 5 (with DSB).
 *   5. cpsid i + nop                          — disable IRQs.
 *   6. Copy 48-vector table (0xC0 bytes) from flash 0x08005028 to
 *      SRAM 0x20000000 via memset_byte_copy.
 *   7. SCB->VTOR (0xE000ED08) = 0x20000000.
 *   8. dsb sy + nop + cpsie i + nop          — re-enable IRQs.
 *   9. peripheral_reset()                     — return value discarded.
 *  10. PWR->CR (0x40007000): clear LPDS bits (10/11), set DBP (bit 12).
 *  11. RCC->APB2ENR |= 1                      — enable SYSCFG clock.
 *  12. RCC->APB1ENR (0x40021038) |= bit28     — enable PWR clock.
 *  13. peripheral_init()  (arg unused).
 *  14. fg_clear_status().
 *  15. dma_init().
 *  16. Magic 0xAAAA via pointer at 0x20002C10: `**(u32 **)0x20002C10 = 0xAAAA`.
 *  17. flash_enable_prefetch().
 *  18. flash_unlock().
 *  19. system_init().
 *
 * The OEM inlines `cpsid i` / `cpsie i` (no wrapper symbols), so this
 * decomp drops the previous `disable_irqs`/`enable_irqs` helpers and
 * uses the CMSIS-style inline asm directly. Same for the vector-table
 * install — OEM has no `set_vector_table` symbol; it inlines the
 * memset_byte_copy + VTOR sequence.
 */
void batteryware_main(void)
{
    nvic_enable_irq_s_dsb(27);

    /* RCC->APB2ENR &= ~bit14 (USART1 clock off). */
    *(volatile uint32_t *)0x40021034 &= 0xFFFFBFFFu;

    /* EXTI->PR = 1 — write-1-to-clear pending EXTI line 0. */
    *(volatile uint32_t *)0x40010414 = 1U;

    nvic_enable_irq_s_dsb(5);

    __asm__ volatile ("cpsid i" : : : "memory");
    __asm__ volatile ("nop");

    /* Copy 48-entry vector table from flash to SRAM. */
    memset_byte_copy((int)0x20000000, (int)0x08005028, (int)0xC0);

    /* SCB->VTOR = SRAM base. */
    *(volatile uint32_t *)0xE000ED08 = 0x20000000U;
    __DSB();
    __asm__ volatile ("nop");
    __asm__ volatile ("cpsie i" : : : "memory");
    __asm__ volatile ("nop");

    (void)peripheral_reset();

    /* PWR->CR: clear LPDS (bits 10..11), set DBP (bit 12). */
    *(volatile uint32_t *)0x40007000 =
        (*(volatile uint32_t *)0x40007000 & 0xFFFFE7FFu) | 0x1000U;

    /* RCC->APB2ENR |= 1 (SYSCFG en). */
    *(volatile uint32_t *)0x40021034 |= 1U;
    /* RCC->APB1ENR |= bit28 (PWR en). */
    *(volatile uint32_t *)0x40021038 |= 0x10000000U;

    peripheral_init(false);  /* arg unused inside */
    fg_clear_status();
    dma_init();

    /* OEM: `ldr r3, [0x20002C10]; ldr r3, [r3]; str 0xAAAA, [r3]` —
     * write 0xAAAA to wherever the pointer at 0x20002C10 points. */
    **(volatile uint32_t **)0x20002C10 = 0x0000AAAAU;

    (void)flash_enable_prefetch();
    flash_unlock();
    system_init();
}

/*
 * Peripheral init — 3-phase USART/DMA/GPIO startup.
 *
 * Phase 1: zero 3 local structs, set RCC bit 0x800,
 *   fill DMA/bus-fault params after memset, then call bus_fault_reset.
 *   system_reset on failure.
 *
 * Phase 2: fill RCC_ClkInitTypeDef (ClockType=0x0F covering all four
 *   AHB/APB/SYSCLK domains, SYSCLKSource=HSI16, AHBCLKDivider=SYSCLK/2),
 *   call rcc_configure with FLatency=0. reset on failure.
 *
 * Phase 3: configure RCC struct, call rcc_reconfigure, reset on failure.
 */
void peripheral_init(bool arg)
{
    uint32_t dma_cfg[14];       /* 0x38 bytes — DMA/bus config */
    uint8_t  usart_cfg[20];     /* 0x14 bytes — USART clock config */
    uint32_t rcc_cfg[9];        /* 0x24 bytes — RCC config */

    (void)arg;

    memset_byte_fill((uint8_t *)dma_cfg,   0, 0x38);
    memset_byte_fill(usart_cfg,            0, 0x14);
    memset_byte_fill((uint8_t *)rcc_cfg,   0, 0x24);

    volatile uint32_t *s_rcc = (volatile uint32_t *)0x20002C6C;
    *s_rcc = (*s_rcc & 0xFFFFFFF7) | 0x800;

    dma_cfg[0] = 10;
    *(uint32_t *)((uint8_t *)dma_cfg + 0x30) = 1;     /* bit-banding / flags */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x2C) = 0x10;  /* prescaler / period */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x28) = 1;     /* enable */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x14) = 1;     /* direction / mode */

    extern int bus_fault_reset(void *);
    if (bus_fault_reset(dma_cfg) != 0) {
        system_reset();
    }

    usart_cfg[0] = 0x0F;        /* ClockType = SYSCLK|HCLK|PCLK1|PCLK2 */
    usart_cfg[4] = 1;           /* SYSCLKSource = HSI16              */
    usart_cfg[8] = 0x80;        /* AHBCLKDivider = SYSCLK / 2        */
    /* bytes 1-3, 5-7, 9-19 stay 0 from memset (APB1/2 div = 1) */

    extern uint32_t rcc_configure(void *cfg, uint32_t flatency);
    if (rcc_configure(usart_cfg, 0) != 0) {
        system_reset();
    }

    rcc_cfg[0] = 1;
    rcc_cfg[1] = 2;

    if (rcc_reconfigure(rcc_cfg) != 0) {
        system_reset();
    }
}

/*
 * Main super-loop — BMS state dispatch.
 *
 * Boot sequence: bms_setup → print banner → check UVP/OVP startup
 * state vs button GPIO → dispatch via voltage comparison to state
 * handlers → enter infinite loop polling flags and dispatching
 * state callbacks via jump table at 0x08005B30.
 *
 * SRAM layout used:
 *   0x20002B58  g_bms_state        current BMS state (0–0x19)
 *   0x20002C00  g_bms_timer        state timer counter
 *   0x20002C44  g_fault_flags      central fault register
 *   0x20002C48  g_fault_shadow     fault shadow/acknowledge
 *   0x20002C50  g_fault_pending    pending fault bits
 *   0x20002C54  g_state_flags      state engine flag bits
 *   0x20002C58  g_cell_status      cell voltage base struct
 *   0x20002C5C  g_state_cfg        state config bits
 *   0x20002C60  g_cmp1             voltage comparator 1 (uint16)
 *   0x20002C64  g_cmp2             voltage comparator 2 (uint16)
 */
void main_loop(void)
{
    extern void bms_setup(void);

    batteryware_main();
    bms_setup();

    volatile uint32_t * const g_timer        = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const g_flags         = (volatile uint32_t *)0x20002C44;
    volatile uint32_t * const g_state        = (volatile uint32_t *)0x20002B58;
    volatile uint8_t  * const g_bms_state    = (volatile uint8_t  *)0x20002B58;

    *g_timer = 0;

    /* Print boot banner */
    uart_puts("I am VanMoof AP\r\n");
    uart_tx_flush();

    /* Load pending faults */
    *g_flags = *(volatile uint32_t *)0x20002C50;

    /* Startup state check: UVP/OVP power-on vs normal boot */
    if (*g_bms_state == 0x17 || *g_bms_state == 0x18) {
        /* UVP/OVP power-on path */
        if (*g_bms_state == 0x17) {
            volatile uint32_t *g_fault_shadow = (volatile uint32_t *)0x20002C48;
            *g_fault_shadow |= 0x40;     /* mark UVP detected */
        }
        volatile uint32_t *g_state_flags = (volatile uint32_t *)0x20002C54;
        *g_state_flags |= 0x8000;
        state_handler_17_19();

        uart_puts("Power On Detect Mode\r\n");
    } else {
        /* Normal boot — check button GPIO */
        bool button_pressed = gpio_bit_read(0x50000000, 0x800);
        volatile uint32_t *g_state_flags = (volatile uint32_t *)0x20002C54;

        if (button_pressed) {
            *g_state_flags |= 8;
            uart_puts("DP Mode\r\n");
        } else {
            *g_state_flags &= ~8U;
            uart_puts("VanMoof Mode\r\n");
        }

        extern void veneer_11f48(void);
        veneer_11f48();

        /* Dispatch to charge/discharge states based on voltage comparators */
        volatile uint32_t *g_cell_status = (volatile uint32_t *)0x20002C58;
        volatile uint32_t *g_state_cfg   = (volatile uint32_t *)0x20002C5C;
        volatile uint16_t *g_cmp1        = (volatile uint16_t *)0x20002C60;
        volatile uint16_t *g_cmp2        = (volatile uint16_t *)0x20002C64;

        if (*g_bms_state == 10 && *g_cmp1 < *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x46)) {
            uart_puts("FEDL5236_Max_Cell_Voltage over threshold\r\n");
            *g_state = 10;
            *g_state_cfg |= 8;
            extern void state_handler_0a(void);
            state_handler_0a();
        } else if (*g_bms_state == 9 && *g_cmp1 < *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x3E)) {
            uart_puts("FEDL5236_Min_Cell_Voltage under threshold\r\n");
            *g_state = 9;
            *g_state_cfg |= 4;
            extern void state_handler_09(void);
            state_handler_09();
        } else if (*g_bms_state == 8 && *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x36) < *g_cmp2) {
            uart_puts("CHG CAL over threshold\r\n");
            *g_state = 8;
            *g_state_cfg |= 2;
            extern void state_handler_08(void);
            state_handler_08();
        } else if (*g_bms_state == 7 && *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x2E) < *g_cmp2) {
            uart_puts("DSG CAL over threshold\r\n");
            *g_state = 7;
            *g_state_cfg |= 1;
            extern void state_handler_07(void);
            state_handler_07();
        } else {
            state_handler_01();
        }
    }

    uart_tx_flush();

    /* Infinite state dispatch loop via jump table at 0x08005B30 */
    void (* const * const g_state_jump_table)(void) =
        (void (* const * const)(void))0x08005B30;

    while (1) {
        /* Check if any fault flag is active; if not, dispatch state timer */
        if (((*g_flags >> 1) & 1) == 0 &&
            ((*g_flags & 1) == 0) &&
            ((*g_flags >> 2) & 1) == 0) {
            if (*g_state < 0x1A) {
                g_state_jump_table[*g_state]();
                return;
            }
            state_handler_01();
        }
        extern void uart_resp_handler(void);
        uart_resp_handler();
        uart_tx_isr();
    }
}