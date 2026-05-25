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

/* GPIO pin reset helper — declared externally (FUN_0800fae0) */
extern void gpio_pin_reset(uint32_t gpio_base, uint32_t pin_mask);

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
    extern void modem_deinit(void *ctx);
    modem_deinit((void *)s_modem_ctx);

    /* Reset USART2 in RCC_APB1RSTR */
    RCC[0x34 / 4] &= 0xFFFFEFFF;

    /* Reset GPIOB pins */
    gpio_pin_reset(0x50000400, 0x38);

    *s_modem_ctx = 0;
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
