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

#endif /* SHIFTER_MODBUS_H */
