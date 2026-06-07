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
