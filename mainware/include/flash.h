#ifndef MAINWARE_FLASH_H
#define MAINWARE_FLASH_H

#include <stdint.h>

/* Shadow-flash staging for OTA. After a firmware PACK is received over BLE and
 * validated (pack_validate, 0x0803CFD8 — magic 0xAA55AA55, length < 0x40000,
 * hardware CRC-32 over the body past the 0x28-byte header), subsystem_update_sm
 * erases the destination sectors and programs the image word-by-word here. */

/* Erase every flash sector overlapping [addr, addr+len). Returns 0, or the HAL
 * error code on failure. OEM flash_erase at 0x0803CF30. */
int flash_erase(int addr, int len);

/* Program `len` bytes (word-aligned) from `data` to flash at `addr`. IRQs are
 * masked across the operation. Returns 0, or the HAL error on the failing word.
 * OEM flash_write at 0x0803CF94. */
int flash_write(uint32_t addr, const uint32_t *data, int len);

#endif
