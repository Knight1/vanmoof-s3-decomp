#include "batteryware.h"

/* USART2 base */
static volatile uint32_t * const USART2 = (volatile uint32_t *)0x40023000;

/* RCC base */
static volatile uint32_t * const RCC    = (volatile uint32_t *)0x40021000;

/* USART config struct in SRAM */
static volatile uint32_t * const s_modem_cfg = (volatile uint32_t *)0x20002C20;

/* Timeout counter */
static volatile uint32_t * const s_modem_timeout = (volatile uint32_t *)0x200047DC;

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
