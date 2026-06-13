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

/* The persisted config record body: 4 "header" words (ctx+0xF4..0x103) passed in
 * registers, then a 0xC0-byte block (ctx+0x104..0x1C3) passed BY VALUE on the
 * stack — matching the OEM's memcpy-then-call ABI. */
struct boot_cfg_block { uint8_t bytes[0xC0]; };

/* Commit the running config record to BOTH redundant internal-flash banks
 * (A @ 0x08008000, B @ 0x0800C000, each one 16 KB sector), so a power loss
 * mid-write can't brick the stored config. Returns the OR of the two per-bank
 * status bytes (0 = both ok). OEM config_persist_dual_bank at 0x08031728. */
uint8_t config_persist_dual_bank(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                 struct boot_cfg_block payload);

/* Commit one config record to a single bank: erase -> CRC -> program -> verify.
 * Stack-aliasing ABI (the 0xD0-byte record is split across the 4 reg args + the
 * by-value payload; bank_dest/size follow). Returns 1 erase / 2 write / 3 verify
 * / 0 ok. OEM flash_config_bank_write at 0x080316D0 (sourced in flash.c). */
uint8_t flash_config_bank_write(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                struct boot_cfg_block payload,
                                uint32_t bank_dest, uint32_t size);

/* Load the config record from flash (bank A, CRC-verify, fall back to bank B and
 * heal bank A on recovery). Returns 0 if either bank was valid, 1 if both are
 * corrupt; `out` must be >= 0xD0 bytes. OEM 0x08031784 (sourced in flash.c). */
int flash_read_config_with_crc_restore(void *out);

#endif
