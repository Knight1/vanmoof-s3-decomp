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

/* Modbus-RTU CRC-16 (poly 0xA001) shared accumulator (SRAM 0x200000C2). */
void     bus_crc16_reset(void);              /* 0x080398E4 */
uint16_t bus_crc16_update(uint16_t byte);    /* 0x080398F4 */
uint16_t bus_crc16_get(void);                /* 0x08039930 */
void     bus_crc16_verify(void);             /* 0x08039954 (bus-RX framing reset) */

#endif
