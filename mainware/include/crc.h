#ifndef MAINWARE_CRC_H
#define MAINWARE_CRC_H

#include <stdint.h>

/* Modbus RTU CRC-16 (reflected polynomial 0xA001), the checksum used across the
 * VanMoof S3 inter-module bus and on the SLIP/BLE frame trailer. */

/* Per-byte CRC update step (OEM crc16_modbus_update, 0x0803C2A8). */
uint16_t crc16_modbus_update(uint16_t crc, uint8_t data);

/* CRC-16 over `len` bytes starting at `buf`, seeded with `crc`
 * (OEM crc16, 0x0803C2C8). */
uint16_t crc16(const uint8_t *buf, int len, uint16_t crc);

#endif
