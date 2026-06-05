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

/* STM32 hardware CRC-32 driver descriptor (the CRC HAL handle at SRAM
 * 0x20009D90). Only the fields the word-feed touches are modelled. */
typedef struct {
    volatile uint32_t *dr;   /* +0x00  -> CRC->DR (CRC peripheral data register) */
    uint8_t  _pad4;          /* +0x04 */
    uint8_t  state;          /* +0x05  driver state: 2 = feeding, 1 = idle/done */
} crc_dev_t;

/* Feed `word_count` 32-bit words from `src` into the STM32 CRC peripheral via
 * `dev` and return the accumulated CRC (OEM crc32_hw_feed, 0x0802320E). */
uint32_t crc32_hw_feed(crc_dev_t *dev, const uint32_t *src, uint32_t word_count);

#endif
