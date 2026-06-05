#ifndef MAINWARE_UART_H
#define MAINWARE_UART_H

#include <stdint.h>

/* Transmit one byte on the serial link (OEM uart_send_byte, 0x080364F0).
 * Pushes the byte into the TX ring buffer with the peripheral TX interrupt
 * masked for atomicity; returns the ring-buffer push status (1 = queued,
 * 0 = ring full). */
int uart_send_byte(uint8_t b);

#endif
