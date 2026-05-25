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
