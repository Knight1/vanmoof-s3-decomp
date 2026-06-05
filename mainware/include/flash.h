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

/* FLASH HAL leaves: program one word (OEM 0x08027A04), and unlock + clear the
 * SR error flags (OEM 0x0803CF1C, the eraser's prep step). */
void flash_program_word(volatile uint32_t *dst, uint32_t value);
void flash_unlock_and_clear_status(void);

/* Map a flash address to its STM32F4 erase-sector index 0..15 (OEM 0x0803CE14). */
uint32_t flash_addr_to_sector(uint32_t flash_addr);

/* Validate a firmware OTA PACK: magic 0xAA55AA55, length < 0x40000, hardware
 * CRC-32 (header-with-CRC/len-masked then body) vs pack[2]. Writes a normalised
 * header copy to out_hdr. Returns 0 ok / 1 CRC mismatch / 2 bad magic-or-length.
 * OEM pack_validate at 0x0803CFD8. */
uint32_t pack_validate(uint32_t *out_hdr, uint32_t *pack);

#endif
