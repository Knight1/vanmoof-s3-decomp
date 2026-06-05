#ifndef MAINWARE_EEPROM_H
#define MAINWARE_EEPROM_H

#include <stdint.h>

/* Write a byte region to the on-board I2C AT24C EEPROM (device 0xA0), splitting
 * the range into <=8-byte page-aligned chunks (5 ms write delay + watchdog kick
 * between pages). Rejects len 0 or a region crossing the 0x80-byte device.
 * Returns 0 on success, 1 on bad args or an I2C error. OEM eeprom_write_region
 * at 0x0803E258 (used by save_state_record_to_eeprom). */
uint32_t eeprom_write_region(uint32_t addr, const uint8_t *src, uint32_t len);

#endif
