#include "batteryware.h"

/* USART2 base */
static volatile uint32_t * const USART2 = (volatile uint32_t *)0x40023000;

/* RCC base */
static volatile uint32_t * const RCC    = (volatile uint32_t *)0x40021000;

/* USART config struct in SRAM */
static volatile uint32_t * const s_modem_cfg = (volatile uint32_t *)0x20002C20;

/* Timeout counter */
static volatile uint32_t * const s_modem_timeout = (volatile uint32_t *)0x200047DC;

/* Modem context struct */
static volatile uint32_t * const s_modem_ctx     = (volatile uint32_t *)0x20002BA4;

/*
 * Configure USART2/modem for communication.
 *
 * Initializes the USART config struct (base = USART2, fields cleared,
 * timeout = 3), enables USART2 clock in RCC_APB1ENR (bit 0x1000),
 * clears the timeout counter, then calls the configuration function.
 * Triggers system reset on failure.
 */
void modem_config(void)
{
    s_modem_cfg[0] = (uint32_t)USART2;   /* base */
    *(volatile uint8_t *)(s_modem_cfg + 1) = 0;  /* field[1] = 0 */
    *(volatile uint8_t *)(s_modem_cfg + 5 / 4) = 0;  /* field[5] = 0 */
    s_modem_cfg[5 / 4] = 0;
    s_modem_cfg[6 / 4] = 0;
    s_modem_cfg[8 / 4] = 3;              /* timeout = 3 */

    /* Enable USART2 clock in RCC_APB1ENR */
    RCC[0x30 / 4] |= 0x1000;

    *s_modem_timeout = 0;

    extern int modem_configure(void *cfg);  /* FUN_0800edf0 */
    if (modem_configure((void *)s_modem_cfg) != 0) {
        system_reset();
    }
}

/*
 * Re-initialize the USART2/modem.
 *
 * Enables NVIC IRQ 0x19 (USART2), sets RCC_APB1ENR bit 0x1000,
 * masks interrupt enable, deinits the modem context via modem_deinit,
 * masks RCC_APB1RSTR, resets GPIOB pins 3-5 (0x38), and clears
 * the modem context pointer.
 */
void modem_reinit(void)
{
    nvic_enable_irq_s_dsb(0x19);

    /* Set USART2 clock bit in RCC_APB1ENR */
    RCC[0x24 / 4] |= 0x1000;
    /* Mask interrupt enable */
    RCC[0x24 / 4] &= 0xFFFFEFFF;

    /* Deinitialize modem */
    modem_deinit((void *)s_modem_ctx);

    /* Reset USART2 in RCC_APB1RSTR */
    RCC[0x34 / 4] &= 0xFFFFEFFF;

    /* Reset GPIOB pins */
    gpio_pin_reset(0x50000400, 0x38);

    *s_modem_ctx = 0;
}

/*
 * Deinitialize the USART/modem context.
 *
 * Sets the context's status byte to 2, clears bit 6 (0x40) in the base
 * register, calls the deinit thunk, then zeroes the status and control
 * fields. Returns true if ctx was null (error), false on success.
 */
bool modem_deinit(void *ctx)
{
    if (ctx != NULL) {
        volatile uint32_t *c = (volatile uint32_t *)ctx;
        *(volatile uint8_t *)((uintptr_t)c + 0x51) = 2;
        *c &= ~0x40U;
        extern void modem_deinit_thunk(void *);
        modem_deinit_thunk(ctx);
        c[0x15] = 0;
        *(volatile uint8_t *)((uintptr_t)c + 0x51) = 0;
        *(volatile uint8_t *)(c + 0x14) = 0;
    }
    return ctx == NULL;
}

/*
 * Bootloader entry sequence.
 *
 * Prints "I am VanMoof AP", turns off charge MOSFET, enters state 6,
 * then calls FUN_080050ac (power-on GPIO check). If the check passes
 * (true), verifies a config block and enters an infinite loop (bootloader
 * mode). Otherwise falls through to state_handler_01 (normal boot).
 */
void bootloader_entry(void)
{
    extern volatile uint8_t * const s_protection_cfg;
    extern void uart_printf(char *str);
    extern void charge_mosfet_off(void);
    extern void bms_set_state(uint8_t state);
    extern void memcmp_verify(char *a, uint32_t size, char *b);
    extern void state_handler_01(void);

    s_protection_cfg[5] = 1;
    *s_modem_ctx |= 0x20000;
    uart_printf((char *)0x08007204);
    uart_tx_flush();
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    bms_configure(0);
    bms_set_state(6);
    uart_printf((char *)0x0800720C);
    uart_tx_flush();

    extern bool power_on_gpio_check(void);  /* FUN_080050ac */
    if (power_on_gpio_check()) {
        memcmp_verify((char *)0x08007214, 0x80, (char *)0x08007210);
        gpio_bit_write(0x50000400, 0x1000, 0);
        while (1) { }  /* bootloader mode — wait for reset */
    }

    *s_modem_ctx &= 0xFFFFFFFD;
    state_handler_01();
}

/*
 * Append 2 bytes to the modem response buffer.
 *
 * Writes two bytes at the current buffer position, increments the
 * index by 2, decrements the remaining count by 2, and increments
 * the outgoing counter.
 */
void modem_send_2bytes(uint8_t b1, uint8_t b2)
{
    volatile uint32_t * const s_buf_idx   = (volatile uint32_t *)0x200047D0;
    volatile uint8_t  * const s_buf_base  = (volatile uint8_t  *)0x20004648;
    volatile uint32_t * const s_buf_rem   = (volatile uint32_t *)0x200047D4;
    volatile uint32_t * const s_tx_count  = (volatile uint32_t *)0x20004748;

    s_buf_base[*s_buf_idx] = b1;
    *s_buf_idx += 1;
    *s_buf_rem -= 1;
    s_buf_base[*s_buf_idx] = b2;
    *s_buf_idx += 1;
    *s_buf_rem -= 1;
    *s_tx_count += 1;
}

/*
 * Temperature offset computation and send.
 *
 * Given a raw temperature byte (0-255):
 *   - If < 40: computes (40 - val) * (-10) → negative offset from 40°C
 *   - If >= 40: computes (val - 40) * 10 → positive offset from 40°C
 * Adds base offset 0x0AAB (2731 = 27.31°C in centikelvin, or a calibration
 * constant) and sends the resulting 16-bit value as two bytes.
 */
void temp_offset_send(uint8_t raw_temp)
{
    uint16_t val = raw_temp;
    int16_t offset;

    if (val < 0x28) {
        offset = (int16_t)((0x28 - val) * (-10));
    } else {
        offset = (int16_t)((val - 0x28) * 10);
    }

    offset += 0x0AAB;

    modem_send_2bytes((uint8_t)(offset >> 8), (uint8_t)offset);
}

/*
 * Bus ready check — returns the status byte at ctx+0x51.
 *
 * The status byte tracks the bus/I²C state machine phase.
 * 0 = idle/ready, 1 = active, 2 = deiniting, etc.
 */
uint8_t bus_ready_check(int ctx)
{
    return *(volatile uint8_t *)(ctx + 0x51);
}

/*
 * Modem restart thunk — empty ROP call site for exception vector dispatch.
 */
void modem_restart_thunk(void)
{
}

/*
 * Modem ISR acknowledgment thunks.
 * Compiler-generated duplicates used as ISR trampolines.
 */
void modem_isr_ack(void)
{
}

void modem_isr_ack_dup(void)
{
}

/*
 * Modem (USART1) initialization.
 *
 * Enables NVIC IRQ 12 (USART1), calls the USART1 config function
 * with the DMA context at 0x200024F4, masks RCC bit 9, resets
 * GPIOA pins 5-7 (0xE0), then zeroes the DMA context pointer.
 */
void modem_init(void)
{
    volatile uint32_t * const s_dma_ctx_init = (volatile uint32_t *)0x200024F4;
    volatile uint32_t * const RCC_INIT       = (volatile uint32_t *)0x40021000;

    nvic_enable_irq_s_dsb(12);

    extern void usart1_dma_setup(void *);  /* FUN_0800e63c */
    usart1_dma_setup((void *)s_dma_ctx_init);

    RCC_INIT[0x34 / 4] &= 0xFFFFFDFF;

    gpio_pin_reset(0x50000000, 0xE0);

    *s_dma_ctx_init = 0;
}
