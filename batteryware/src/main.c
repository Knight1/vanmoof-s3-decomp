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
 * Peripheral init — classic HAL clock bring-up sequence.
 *
 * Phase 1: oscillators (rcc_osc_config / HAL_RCC_OscConfig).
 * Phase 2: clock tree   (rcc_configure / HAL_RCC_ClockConfig).
 * Phase 3: peripheral clocks (rcc_reconfigure / HAL_RCCEx_PeriphCLKConfig).
 *
 * The three local cfg buffers are zeroed first (so unset fields read
 * as the HAL "off / div1" defaults), then the few fields the OEM cares
 * about are filled in before each call. The previous decomp labelled
 * these `dma_cfg` / `usart_cfg` / `rcc_cfg` and routed Phase 1 through
 * a `bus_fault_reset` stub — both names were leftover from when the
 * three calls were thought to be DMA / USART / fault setup.
 */
void peripheral_init(bool arg)
{
    uint32_t osc_cfg[14];       /* 0x38 bytes — rcc_osc_init_t */
    uint8_t  clk_cfg[20];       /* 0x14 bytes — rcc_clk_init_t */
    uint32_t periphclk_cfg[9];  /* 0x24 bytes — peripheral-clock cfg */

    (void)arg;

    memset_byte_fill((uint8_t *)osc_cfg,       0, 0x38);
    memset_byte_fill(clk_cfg,                  0, 0x14);
    memset_byte_fill((uint8_t *)periphclk_cfg, 0, 0x24);

    volatile uint32_t *s_rcc = (volatile uint32_t *)0x20002C6C;
    *s_rcc = (*s_rcc & 0xFFFFFFF7) | 0x800;

    /* Oscillator request: HSI + LSI, plus PLL configured from HSI. */
    osc_cfg[0] = 10;                                            /* OscType: HSI|LSI */
    *(uint32_t *)((uint8_t *)osc_cfg + 0x0C) = 1;               /* HSIState     = ON   */
    *(uint32_t *)((uint8_t *)osc_cfg + 0x14) = 1;               /* LSIState     = ON   */
    *(uint32_t *)((uint8_t *)osc_cfg + 0x28) = 2;               /* PLLState     = ON   */
    *(uint32_t *)((uint8_t *)osc_cfg + 0x2C) = 0x10;            /* PLLSource           */
    *(uint32_t *)((uint8_t *)osc_cfg + 0x30) = 1;               /* PLLMUL              */

    if (rcc_osc_config(osc_cfg) != 0) {
        system_reset();
    }

    clk_cfg[0] = 0x0F;          /* ClockType = SYSCLK|HCLK|PCLK1|PCLK2 */
    clk_cfg[4] = 1;             /* SYSCLKSource = HSI16              */
    clk_cfg[8] = 0x80;          /* AHBCLKDivider = SYSCLK / 2        */
    /* bytes 1-3, 5-7, 9-19 stay 0 from memset (APB1/2 div = 1) */

    extern uint32_t rcc_configure(void *cfg, uint32_t flatency);
    if (rcc_configure(clk_cfg, 0) != 0) {
        system_reset();
    }

    periphclk_cfg[0] = 1;       /* PeriphClockSelection = USART1 */
    periphclk_cfg[2] = 2;       /* Usart1ClockSelection = HSI16  */

    if (rcc_reconfigure(periphclk_cfg) != 0) {
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
int main(void)
{
    extern void bms_setup(void);

    batteryware_main();
    bms_setup();

    volatile uint32_t * const g_flags     = (volatile uint32_t *)0x20002C44; /* used by the dispatch loop below */
    volatile uint32_t * const g_state     = (volatile uint32_t *)0x20002B58; /* used by the dispatch loop below */
    volatile uint8_t  * const g_bms_state = (volatile uint8_t  *)0x20002B58; /* live state byte (handler index) */

    volatile uint32_t * const s_status    = (volatile uint32_t *)0x20002C00;
    volatile uint16_t * const s_prot      = (volatile uint16_t *)0x2000286C;
    volatile uint8_t  * const cfg_blk     = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t * const thr_uvp     = (volatile uint16_t *)0x2000282A;
    volatile uint16_t * const thr_ovp     = (volatile uint16_t *)0x200027FA;
    volatile uint8_t  * const g_boot_mode = (volatile uint8_t  *)0x20002C48;

    *(volatile uint16_t *)0x20002C70 = 0;

    /* Boot banner. The OEM prints from one byte past the shared
     * " \nI am VanMoof AP\r" string, i.e. without the leading space. */
    uart_printf((uint8_t *)&s_i_am_vanmoof_ap_lead[1]);
    uart_tx_flush();

    /* Latch the power-on mode persisted in external flash. */
    *g_boot_mode = *(volatile uint8_t *)0x08080001;

    extern void state_handler_07(void);
    extern void state_handler_08(void);
    extern void state_handler_09(void);
    extern void state_handler_0a(void);

    if (*g_boot_mode == 0x17 || *g_boot_mode == 0x18) {
        /* Hard "MOS Failure" power-on path. */
        if (*g_boot_mode == 0x17) {
            *(volatile uint16_t *)0x20002C44 |= 0x40;   /* g_fault_flags bit 6 */
        }
        *s_status |= 0x8000;
        state_handler_17_19();
        uart_printf((uint8_t *)s_mos_failure_mode);
    } else {
        /* Normal boot — DP vs VanMoof selected by the button on GPIOB PB11. */
        if (gpio_bit_read(0x50000400, 0x800)) {
            *s_status |= 8;
            uart_printf((uint8_t *)s_vanmoof_mode);
        } else {
            *s_status &= ~8U;
            uart_printf((uint8_t *)s_dp_mode);
        }

        extern void veneer_11f48(void);
        veneer_11f48();

        /* Power-on protection detect: print the matching mode, latch the
         * live-state byte + the protection bit, and enter the handler.
         * cfg_blk threshold fields are compared against the UVP/OVP limits
         * (0x2000282A / 0x200027FA). */
        if (*g_boot_mode == 10 && *thr_uvp < *(volatile uint16_t *)(cfg_blk + 0x46)) {
            uart_printf((uint8_t *)s_power_on_uvp2_mode);
            *g_bms_state = 10; *s_prot |= 8; state_handler_0a();
        } else if (*g_boot_mode == 9 && *thr_uvp < *(volatile uint16_t *)(cfg_blk + 0x3E)) {
            uart_printf((uint8_t *)s_power_on_uvp1_mode);
            *g_bms_state = 9;  *s_prot |= 4; state_handler_09();
        } else if (*g_boot_mode == 8 && *(volatile uint16_t *)(cfg_blk + 0x36) < *thr_ovp) {
            uart_printf((uint8_t *)s_power_on_ovp2_mode);
            *g_bms_state = 8;  *s_prot |= 2; state_handler_08();
        } else if (*g_boot_mode == 7 && *(volatile uint16_t *)(cfg_blk + 0x2E) < *thr_ovp) {
            uart_printf((uint8_t *)s_power_on_ovp1_mode);
            *g_bms_state = 7;  *s_prot |= 1; state_handler_07();
        } else if (*thr_uvp <= *(volatile uint16_t *)(cfg_blk + 0x42)) {
            uart_printf((uint8_t *)s_pwron_detect_uvp2);
            *g_bms_state = 10; *s_prot |= 8; state_handler_0a();
        } else if (*thr_uvp <= *(volatile uint16_t *)(cfg_blk + 0x3A)) {
            uart_printf((uint8_t *)s_pwron_detect_uvp1);
            *g_bms_state = 9;  *s_prot |= 4; state_handler_09();
        } else if (*(volatile uint16_t *)(cfg_blk + 0x32) <= *thr_ovp) {
            uart_printf((uint8_t *)s_pwron_detect_ovp2);
            *g_bms_state = 8;  *s_prot |= 2; state_handler_08();
        } else if (*(volatile uint16_t *)(cfg_blk + 0x2A) <= *thr_ovp) {
            uart_printf((uint8_t *)s_pwron_detect_ovp1);
            *g_bms_state = 7;  *s_prot |= 1; state_handler_07();
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
                return 0;
            }
            state_handler_01();
        }
        extern void uart_resp_handler(void);
        uart_resp_handler();
        uart_tx_isr();
    }
}