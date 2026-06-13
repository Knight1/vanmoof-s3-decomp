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

/* CubeF4 CRC HAL MSP hooks (OEM 0x08040288 / 0x080402B8): gate the CRC
 * peripheral clock (RCC_AHB1ENR bit 12, CRCEN) on/off. Both guard on the
 * handle's Instance (the +0 word, which is the CRC base 0x40023000 — also the
 * DR pointer, since CRC->DR sits at offset 0). */
void HAL_CRC_MspInit(crc_dev_t *hcrc);
void HAL_CRC_MspDeInit(crc_dev_t *hcrc);

/* Accumulate a CRC-32 over the 96-bit STM32 device unique ID (0x1FFF7A10) using
 * the shared CRC handle (SRAM 0x20009D90); returns the running CRC. Used by the
 * console `show` command. OEM 0x080402E8. */
uint32_t crc_accumulate_device_uid(void);

#endif
