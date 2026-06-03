#include "powerbankware.h"

/* IWDG kick: the handle cell at 0x200006ac holds the IWDG Instance pointer;
 * writing the reload key (0xAAAA) through it refreshes the watchdog. */
#define IWDG_KR  (**(volatile uint32_t **)0x200006ac)

/*
 * delay_ms — OEM FUN_08013ad4.
 *
 * Busy-waits `ms` software ticks. The SysTick ISR sets bit 0 of the flag
 * byte at 0x2000077c every millisecond; this loop spins until that bit is
 * set, clears it, and decrements. Bit 2 is a slower periodic flag: on the
 * iterations where it is set, the loop clears it and kicks the IWDG by
 * writing the reload key (0xAAAA) through the watchdog pointer cell at
 * 0x200006ac (installed during HAL bring-up).
 */
void delay_ms(int ms)
{
    volatile uint8_t * const tick = (volatile uint8_t *)0x2000077c;

    while (ms != 0) {
        while ((*tick & 1) == 0) {
            /* wait for the next 1 ms tick */
        }
        *tick &= (uint8_t)~1u;
        ms--;

        if ((*tick & 4) != 0) {
            *tick &= (uint8_t)~4u;
            IWDG_KR = 0x0000AAAAu;  /* IWDG_KR reload */
        }
    }
}

/*
 * delay_ms_service — OEM FUN_08013b38.
 *
 * Like delay_ms, but keeps the comms link alive while waiting: each spin it
 * services the UART TX ring and the RX/command processor, and on every 1 ms
 * tick it decrements the counter and kicks the IWDG. Used by the slow
 * EEPROM write/verify retries so the host link doesn't stall.
 */
void delay_ms_service(int ms)
{
    volatile uint8_t * const tick = (volatile uint8_t *)0x2000077c;

    while (ms != 0) {
        if ((*tick & 1) != 0) {
            *tick &= (uint8_t)~1u;
            ms--;
            IWDG_KR = 0x0000AAAAu;   /* IWDG_KR reload */
        }
        uart_tx_isr();
        uart_rx_handler();
    }
}
