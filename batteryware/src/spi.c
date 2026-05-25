#include "batteryware.h"

/* SPI mutex and context */
static volatile uint8_t  * const s_spi_mutex  = (volatile uint8_t *)0x200047E0;
static volatile uint32_t * const s_spi_addr   = (volatile uint32_t *)0x200047E4;
static volatile uint32_t * const s_spi_busy   = (volatile uint32_t *)0x20002000;

/*
 * Write to an SPI register (byte, halfword, or word) with mutex guarding.
 *
 * Type dispatch:
 *   0 → byte write        *(char *)param_2 = (char)param_3
 *   1 → halfword write    *(short *)param_2 = (short)param_3
 *   2 → word write        *param_2 = param_3
 *
 * Returns 0 on success, 1 on mutex contention, 2 if already locked.
 * Used by memcmp_verify to write SPI config bytes to the FEDL5236.
 */
uint8_t spi_register_write(uint8_t type, volatile void *reg, uint32_t val)
{
    extern uint8_t dma_lock(void *ctx);  /* FUN_0800f3ac */

    if (s_spi_mutex[0x10] == 1) {
        return 2;  /* mutex already locked */
    }

    s_spi_mutex[0x10] = 1;
    uint8_t ret = dma_lock((void *)s_spi_busy);

    if (ret == 0) {
        s_spi_addr[0x14 / 4] = 0;

        if (type == 2) {
            *(volatile uint32_t *)reg = val;
        } else if (type == 1) {
            *(volatile uint16_t *)reg = (uint16_t)val;
        } else if (type == 0) {
            *(volatile uint8_t *)reg = (uint8_t)val;
        } else {
            ret = 1;
        }

        if (ret == 0) {
            ret = dma_lock((void *)s_spi_busy);
        }
    }

    s_spi_mutex[0x10] = 0;
    return ret;
}
