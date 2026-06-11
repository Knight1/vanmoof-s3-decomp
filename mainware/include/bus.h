#ifndef MAINWARE_BUS_H
#define MAINWARE_BUS_H

#include <stdint.h>

/* UART4 ("bus") byte transport — the second inter-module serial link, carrying
 * Modbus-RTU traffic to the battery BMS (slave 0xAA). battery.c is the protocol
 * owner; these are the raw byte primitives and the interrupt service routine. */

/* Locked single-byte TX into the UART4 TX ring; implicitly returns
 * ringbuf_push_byte's status (1 = pushed, 0 = ring full). OEM 0x0803639C. */
int bus_tx_enqueue_byte(uint8_t b);

/* Locked single-byte RX from the UART4 RX ring into *out; implicitly returns
 * ringbuf_get_byte's status (1 = byte produced, 0 = ring empty). OEM 0x080363EC. */
int bus_rx_byte_locked(uint8_t *out);

/* UART4 RX/TX byte-pump interrupt service routine (OEM 0x08036424), invoked via a
 * thin vector trampoline. */
void uart4_irq_handler(void);

#endif
