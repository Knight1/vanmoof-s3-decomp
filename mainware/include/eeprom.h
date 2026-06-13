#ifndef MAINWARE_EEPROM_H
#define MAINWARE_EEPROM_H

#include <stdint.h>

/* Write a byte region to the on-board I2C AT24C EEPROM (device 0xA0), splitting
 * the range into <=8-byte page-aligned chunks (5 ms write delay + watchdog kick
 * between pages). Rejects len 0 or a region crossing the 0x80-byte device.
 * Returns 0 on success, 1 on bad args or an I2C error. OEM eeprom_write_region
 * at 0x0803E258 (used by save_state_record_to_eeprom). */
uint32_t eeprom_write_region(uint32_t addr, const uint8_t *src, uint32_t len);

/* Read the 6-byte ID / security ("lock state") block: command 0xFA then a
 * 6-byte read from device 0xA0. Returns the OR of both I2C status bytes (0 =
 * OK). OEM eeprom_read_id_block, 0x0803E138 (= Security_GetLockState in 1.9.x). */
int eeprom_read_id_block(uint8_t *out6);

/* Bounded read of `len` bytes from byte-address `addr` (rejects len 0 or a read
 * past the 0x80-byte device). Returns the HAL status (0 = OK) / 1 on bad args.
 * OEM 0x0803E174. */
int eeprom_read_bounded(uint32_t addr, uint8_t *out, uint32_t len);

#endif
