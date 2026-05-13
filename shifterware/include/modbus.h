#ifndef SHIFTER_MODBUS_H
#define SHIFTER_MODBUS_H

#include <stdint.h>

/* The S3's inter-module bus is Modbus RTU. CRC-16 over each PDU is
 * computed with poly 0xA001 and seed 0xFFFF, then the two CRC bytes
 * are appended low-then-high to the PDU before transmission. */

/* Compute Modbus-RTU CRC-16 over `len` bytes of `buf`. Result is
 * written to the two adjacent module-local scratch bytes at RAM
 * 0x200000E7 (lo) and 0x200000E8 (hi). */
void modbus_crc16_compute(const uint8_t *buf, int len);

/* Send `len` bytes from `buf` byte-at-a-time over UART1 (the OEM's
 * Modbus PHY). Blocks until each byte is fully transmitted. */
void modbus_send_bytes(const uint8_t *buf, unsigned len);

/* Transmit the module-local TX buffer (`len` bytes) over UART1.
 * As a side effect: when `len == 7` AND the firmware-OK latch is
 * set, issue SYSRESETREQ after the transmit so shifterboot can
 * install the freshly-validated image. */
void modbus_tx_finalize(unsigned len);

/* Decrement the Modbus inter-byte timeout counter. Wire this into a
 * periodic ISR (SysTick or the bus-frame timer). */
void modbus_tick(void);

/* Build and send an 8-byte reply: echo the first 6 bytes of the
 * inbound PDU buffer, then 2 bytes of CRC. Used by the RX dispatcher
 * for passthrough/echo responses. */
void modbus_reply_passthrough(void);

/* Top-of-stack PDU dispatcher: called by the RX FSM once a full
 * inbound frame is in the scratch at `0x200000C8`. `cmd` is the
 * function-code byte; `len` is the inbound PDU length. */
void modbus_dispatch_pdu(uint8_t cmd, uint8_t len);

/* RX state machine. Called from the main poll loop; consumes bytes
 * the IRQ has appended to the scratch buffer, applies the
 * end-of-frame timeout, CRC-validates, and forwards complete frames
 * to `modbus_dispatch_pdu`. */
void modbus_rx_poll(void);

#endif /* SHIFTER_MODBUS_H */
