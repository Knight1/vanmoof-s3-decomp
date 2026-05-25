#include "batteryware.h"

/*
 * Batteryware main entry point — startup sequence after Reset_Handler.
 *
 * 1. Enable NVIC IRQ 27 (USB/USART wakeup)
 * 2. Mask RCC bit 9 (power-down domain reset)
 * 3. Configure SRAM parity check
 * 4. Enable NVIC IRQ 5 (RCC)
 * 5. Disable interrupts globally
 * 6. Set VTOR to 0x20000000 (SRAM vector table redirect)
 * 7. Set USART baud rate register
 * 8. DSB + re-enable interrupts
 * 9. Call peripheral_reset
 * 10. Enable IOPAEN clock + USART2 in RCC
 * 11. peripheral_init
 * 12. Fuel gauge init, DMA init
 * 13. Set flash prefetch + unlock + system_init
 */
void batteryware_main(void)
{
    volatile uint32_t * const RCC      = (volatile uint32_t *)0x40021000;
    volatile uint32_t * const s_cfg    = (volatile uint32_t *)0x20002C60;
    volatile uint32_t * const s_regs   = (volatile uint32_t *)0x20002C64;

    nvic_enable_irq_s_dsb(27);

    RCC[0x34 / 4] &= 0xFFFFFDFF;

    s_cfg[0x14 / 4] = 1;

    nvic_enable_irq_s_dsb(5);

    extern void disable_irqs(void);
    disable_irqs();

    extern void set_vector_table(uint32_t, uint32_t, uint32_t);
    set_vector_table(0x20000000, 0x020000C0, 0xC0);

    s_regs[2] = 0x20000000;

    __DSB();
    extern void enable_irqs(void);
    enable_irqs();

    bool reset_done = peripheral_reset();

    *s_regs = (*s_regs & 0xFFFFEFFF) | 0x1000;

    RCC[0x34 / 4] |= 1;
    RCC[0x38 / 4] |= 0x10000000;

    peripheral_init(reset_done);
    extern void fg_init(void);
    fg_init();
    dma_init();

    volatile uint32_t * const s_flash_cfg = (volatile uint32_t *)0x20002C68;
    *s_flash_cfg = 0x00000005;

    flash_enable_prefetch();
    extern void flash_unlock(void);
    flash_unlock();
    system_init();
}

/*
 * Peripheral initialization — 3-phase USART/DMA/GPIO startup.
 *
 * Phase 1: memset three local structs (0x38, 0x14, 0x24 bytes) to 0,
 *   OR RCC bit 0x800, configure bus fault params (10,1,16,1,1),
 *   call FUN_0800fdac (bus fault reset). system_reset on failure.
 *
 * Phase 2: configure 20-byte USART struct (prescaler=0xF, flags_at_4=1,
 *   bit7_at_8=0x80), call FUN_08010554. system_reset on failure.
 *
 * Phase 3: configure RCC struct (flags=1, ?, 2), call rcc_reconfigure.
 *   system_reset on failure.
 */
void peripheral_init(bool arg)
{
    uint32_t cfg_a[0xE];    /* 0x38 bytes */
    uint8_t  cfg_b[0x14];   /* 20 bytes */
    uint32_t cfg_c[9];      /* 0x24 bytes (9 words) */

    (void)arg;
    memset_byte_fill((uint8_t *)cfg_a, 0, 0x38);
    memset_byte_fill(cfg_b, 0, 0x14);
    memset_byte_fill((uint8_t *)cfg_c, 0, 0x24);

    volatile uint32_t * const s_rcc = (volatile uint32_t *)0x20002C6C;
    *s_rcc = (*s_rcc & 0xFFFFFFF7) | 0x800;

    cfg_a[0] = 10;
    *(uint32_t *)((uint8_t *)cfg_a + 0x30) = 1;
    *(uint32_t *)((uint8_t *)cfg_a + 0x2C) = 0x10;
    *(uint32_t *)((uint8_t *)cfg_a + 0x28) = 1;
    *(uint32_t *)((uint8_t *)cfg_a + 0x14) = 1;

    volatile uint32_t * const s_bus_fault = (volatile uint32_t *)0x20002C70;
    *s_bus_fault = 0;

    extern int bus_fault_reset(void *);
    if (bus_fault_reset(cfg_a) != 0) {
        system_reset();
    }

    cfg_b[0] = 0x0F;
    cfg_b[1] = 0;
    cfg_b[2] = 0;
    cfg_b[3] = 0;
    cfg_b[4] = 1;
    cfg_b[5] = 0;
    cfg_b[6] = 0;
    cfg_b[7] = 0;
    cfg_b[8] = 0x80;
    /* bytes 9-19 remain 0 from memset */

    extern int usart_bus_config(void *, uint8_t);
    if (usart_bus_config(cfg_b, 0) != 0) {
        system_reset();
    }

    cfg_c[0] = 1;
    cfg_c[1] = 2;

    if (rcc_reconfigure(cfg_c) != 0) {
        system_reset();
    }
}

/*
 * Main super-loop — post-boot dispatch.
 *
 * Called after batteryware_main. Runs bms_setup, prints startup
 * message, checks startup state (0x17/0x18 for UVP/OVP power-on),
 * or dispatches to charge/discharge states based on voltage levels.
 * Enters infinite loop dispatching state timer callbacks via jump table.
 */
void main_loop(void)
{
    extern void bms_setup(void);

    batteryware_main();
    bms_setup();

    volatile uint32_t * const s_timer   = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const s_flags   = (volatile uint32_t *)0x20002C44;
    volatile uint32_t * const s_state   = (volatile uint32_t *)0x20002B58;
    volatile uint8_t  * const s_st      = (volatile uint8_t  *)0x20002B58;

    *s_timer = 0;
    extern void uart_printf(char*);
    uart_printf((char*)0x08016FD0);
    uart_tx_flush();

    *s_flags = *(volatile uint32_t *)0x20002C50;

    if (*s_st == 0x17 || *s_st == 0x18) {
        if (*s_st == 0x17) {
            volatile uint32_t *s = (volatile uint32_t *)0x20002C48;
            *s |= 0x40;
        }
        volatile uint32_t *s2 = (volatile uint32_t *)0x20002C54;
        *s2 |= 0x8000;
        state_handler_17_19();
        uart_printf((char*)0x08016FEC);
    } else {
        bool btn = gpio_bit_read(0x50000000, 0x800);
        if (btn) {
            volatile uint32_t *s = (volatile uint32_t *)0x20002C54;
            *s |= 8;
            uart_printf((char*)0x08017030);
        } else {
            volatile uint32_t *s = (volatile uint32_t *)0x20002C54;
            *s &= ~8U;
            uart_printf((char*)0x08017010);
        }

        extern void veneer_11f48(void);
        veneer_11f48();

        volatile uint32_t *s_vol  = (volatile uint32_t *)0x20002C58;
        volatile uint32_t *s_cfg  = (volatile uint32_t *)0x20002C5C;
        volatile uint16_t *s_cmp1 = (volatile uint16_t *)0x20002C60;
        volatile uint16_t *s_cmp2 = (volatile uint16_t *)0x20002C64;
        volatile uint16_t *s_cmp3 = (volatile uint16_t *)0x20002C68;

        if (*s_st == 10 && *s_cmp1 < *(volatile uint16_t *)((uint8_t *)s_vol + 0x46)) {
            uart_printf((char*)0x08017044);
            *s_state = 10;
            *s_cfg |= 8;
            state_handler_0a();
        } else if (*s_st == 9 && *s_cmp1 < *(volatile uint16_t *)((uint8_t *)s_vol + 0x3E)) {
            uart_printf((char*)0x0801705C);
            *s_state = 9;
            *s_cfg |= 4;
            state_handler_09();
        } else if (*s_st == 8 && *(volatile uint16_t *)((uint8_t *)s_vol + 0x36) < *s_cmp2) {
            uart_printf((char*)0x08017074);
            *s_state = 8;
            *s_cfg |= 2;
            state_handler_08();
        } else if (*s_st == 7 && *(volatile uint16_t *)((uint8_t *)s_vol + 0x2E) < *s_cmp2) {
            uart_printf((char*)0x0801708C);
            *s_state = 7;
            *s_cfg |= 1;
            state_handler_07();
        } else {
            /* default dispatch via state lookup */
            state_handler_01();
        }
    }

    uart_tx_flush();

    /* Infinite dispatch loop */
    void (* const * const s_jt)(void) = (void (* const * const)(void))0x08005B30;
    while (1) {
        if ((((*s_flags >> 1) & 1) == 0) && ((*s_flags & 1) == 0) &&
            (((*s_flags >> 2) & 1) == 0)) {
            if (*s_state < 0x1A) {
                s_jt[*s_state]();
                return;
            }
            state_handler_01();
        }
        extern void uart_resp_handler(void);
        uart_resp_handler();
        uart_tx_isr();
    }
}
