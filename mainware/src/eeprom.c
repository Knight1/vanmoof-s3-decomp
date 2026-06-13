#include <stdint.h>

#include "eeprom.h"
#include "systick.h"

/* On-board I2C AT24C EEPROM region writer (OEM eeprom_write_region, 0x0803E258).
 * The device has 8-byte pages; a write that would cross a page boundary is split
 * so each HAL_I2C_Mem_Write stays within one page. The EEPROM address is encoded
 * as `offset | ((page & 0x1FFF) << 3)`. */

extern void *g_eeprom_i2c_handle;   /* SRAM 0x20009B04: I2C HAL handle */
extern void  watchdog_kick(void);   /* 0x080314D8 */

/* CubeF4 HAL_I2C_Mem_Write(handle, dev_addr, mem_addr, mem_addr_size, data,
 * size, timeout) — OEM HAL_I2C_Mem_Write. 0 = HAL_OK. */
extern int HAL_I2C_Mem_Write(void *handle, uint16_t dev_addr, uint16_t mem_addr,
                             uint16_t mem_addr_size, const uint8_t *data,
                             uint16_t size, uint32_t timeout);
extern int HAL_I2C_Mem_Read(void *handle, uint16_t dev_addr, uint16_t mem_addr,
                            uint16_t mem_addr_size, uint8_t *data,
                            uint16_t size, uint32_t timeout);
extern uint32_t HAL_I2C_Master_Transmit(void *handle, uint16_t dev_addr,
                                        const uint8_t *data, uint16_t size, uint32_t timeout);
extern uint32_t HAL_I2C_Master_Receive(void *handle, uint16_t dev_addr,
                                       uint8_t *data, uint16_t size, uint32_t timeout);

uint32_t eeprom_write_region(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t page;
    uint32_t offset;
    uint32_t chunk;

    if (len == 0) {
        return 1;
    }
    if ((int32_t)(addr + len) >= 0x81) {       /* region must fit the 0x80-byte device */
        return 1;
    }

    page   = addr >> 3;
    offset = addr & 7;

    chunk = len;
    if (offset + len > 8) {                     /* first chunk: up to the page edge */
        chunk = 8 - offset;
    }

    while (len != 0) {
        uint32_t mem_addr = offset | ((page & 0x1FFF) << 3);

        if (HAL_I2C_Mem_Write(g_eeprom_i2c_handle, 0xA0, (uint16_t)mem_addr, 1,
                              src, (uint16_t)chunk, 0x32) != 0) {
            return 1;
        }

        systick_delay(5);                       /* AT24C self-timed write cycle */
        watchdog_kick();

        src    += chunk;
        len    -= chunk;
        page   += 1;
        offset  = 0;
        chunk   = (len > 8) ? 8 : len;
    }

    return 0;
}

/* Read the 6-byte ID / security block from the EEPROM (device 0xA0): send the
 * command byte 0xFA, then read 6 bytes into `out6`. Returns the OR of the two
 * I2C status bytes (0 = OK). Probed once during boot
 * (mainware_boot_init_sequence). In later VanMoof firmware (1.9.x) this same
 * routine is named Security_GetLockState — the 6 bytes are the bike's
 * lock/security state. OEM eeprom_read_id_block, 0x0803E138. */
int eeprom_read_id_block(uint8_t *out6)
{
    uint8_t cmd[5];
    uint32_t e1, e2;

    cmd[0] = 0xFA;
    e1 = HAL_I2C_Master_Transmit(g_eeprom_i2c_handle, 0xA0, cmd, 1, 0x32);
    e2 = HAL_I2C_Master_Receive(g_eeprom_i2c_handle, 0xA0, out6, 6, 0x32);
    return (int)((e2 | e1) & 0xFF);
}

/* Bounded EEPROM read: `len` bytes from byte-address `addr` (1-byte memory
 * addressing). Rejects len 0 or a read crossing the 0x80-byte device. Returns
 * the HAL_I2C_Mem_Read status (0 = OK) or 1 on bad args. OEM 0x0803E174 (the
 * read primitive behind eeprom_read_config_with_crc_fallback). */
int eeprom_read_bounded(uint32_t addr, uint8_t *out, uint32_t len)
{
    if (len == 0) {
        return 1;
    }
    if (addr + len > 0x80) {
        return 1;
    }
    return HAL_I2C_Mem_Read(g_eeprom_i2c_handle, 0xA0, (uint16_t)addr, 1, out,
                            (uint16_t)len, 0x32);
}
