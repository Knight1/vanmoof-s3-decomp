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
