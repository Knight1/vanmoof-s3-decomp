#include "batteryware.h"

/* SPI mutex (0x200047E0): [0x10] = held flag, [0x14] = cleared on acquire */
static volatile uint8_t  * const s_spi_mutex  = (volatile uint8_t *)0x200047E0;

/*
 * Write a register byte/halfword/word, guarded by the SPI mutex and the
 * DMA/EEPROM ready poll.
 *
 * `type` selects the access width: 0 = byte, 1 = halfword, 2 = word (the
 * OEM encoding — note it is the inverse of a width-in-bytes value). The
 * caller commits a value and, by re-reading `reg`, confirms it landed;
 * `memcmp_verify` loops on this until the read-back matches.
 *
 * Returns 0 on success, 2 if the mutex is already held, 3 on ready-poll
 * timeout, 1 on an invalid `type`.
 */
uint8_t spi_register_write(uint8_t type, volatile void *reg, uint32_t val)
{
    uint8_t ret = 1;

    if (s_spi_mutex[0x10] == 1) {
        return 2;
    }

    s_spi_mutex[0x10] = 1;
    ret = (uint8_t)dma_wait_for_ready(50000);

    if (ret == 0) {
        *(volatile uint32_t *)&s_spi_mutex[0x14] = 0;

        if (type == 2) {
            *(volatile uint32_t *)reg = val;
        } else if (type == 1) {
            *(volatile uint16_t *)reg = (uint16_t)val;
        } else if (type == 0) {
            *(volatile uint8_t *)reg = (uint8_t)val;
        } else {
            ret = 1;
        }

        if (ret != 0) {
            ret = (uint8_t)dma_wait_for_ready(50000);
        }
    }

    s_spi_mutex[0x10] = 0;
    return ret;
}

/*
 * EEPROM/SPI register write-verify (FUN_080093a6).
 *
 * For each byte, commit `expected[i]` to the register/EEPROM cell at
 * `&actual[i]` via spi_register_write(0, …) and re-read until the value
 * sticks. `actual` is the destination that is read back (r0); `expected`
 * holds the source bytes to commit (r2). Used by every calibration/config
 * persist path to push a RAM image out to the FEDL5236 EEPROM block.
 */
void memcmp_verify(char *actual, uint32_t len, char *expected)
{
    char *dst = actual;
    char *src = expected;

    for (uint32_t i = 0; i < len; i++) {
        do {
            spi_register_write(0, dst, (uint8_t)*src);
        } while (*src != *dst);
        dst++;
        src++;
    }
}

/*
 * SMBus write register — write val to FEDL5236 register reg with mask.
 *
 * Core FEDL5236 communication primitive.
 * Retries CRC mismatches, system_reset on transmit failure.
 */
void smbus_write_reg(uint8_t reg, uint8_t val, uint8_t mask)
{
    volatile uint8_t  * const s_ctx     = (volatile uint8_t  *)0x20002BA4;
    volatile uint8_t  * const s_cfg_reg = (volatile uint8_t  *)0x20002C88;
    volatile uint8_t  * const s_tx_buf  = (volatile uint8_t  *)0x20002B64;
    volatile uint32_t * const s_tx_len  = (volatile uint32_t *)0x20002B61;
    volatile uint8_t  * const s_rx_buf  = (volatile uint8_t  *)0x20002B84;
    volatile uint8_t  * const s_ret_val = (volatile uint8_t  *)0x20002B7C;
    bool retry;

    do {
        uint8_t attempt = 0;
        retry = false;

        while (bus_ready_check((int)s_ctx) != 1) { }

        if (reg == 9) {
            *s_cfg_reg = val;
        }

        s_tx_buf[0] = (reg << 1) | 0x80;
        s_tx_buf[1] = val;
        s_tx_buf[2] = (uint8_t)crc8_for_smbus((uint8_t *)s_tx_buf, 2);
        *s_tx_len = 3;

        gpio_bit_write(0x50000000, 0x8000, 0);

        if (smbus_transmit((int *)s_ctx, (int)s_tx_buf, (int)s_rx_buf, (int16_t)*s_tx_len) != 0) {
            system_reset();
        }

        do {
            while (bus_ready_check((int)s_ctx) != 1) { }
            s_tx_buf[0] = (reg << 1) | 0x81;
            s_tx_buf[1] = 1;
            s_tx_buf[2] = 0xFF;
            *s_tx_len = 4;

            gpio_bit_write(0x50000000, 0x8000, 0);
            if (smbus_transmit((int *)s_ctx, (int)s_tx_buf, (int)s_rx_buf, (int16_t)*s_tx_len) != 0) {
                system_reset();
            }

            do { } while (bus_ready_check((int)s_ctx) != 1);
            s_rx_buf[0] = s_tx_buf[0];
            s_rx_buf[1] = s_tx_buf[1];
            attempt++;
            if (attempt > 9) system_reset();
        } while (crc8_verify((uint8_t *)s_rx_buf, 3) != s_rx_buf[3]);

        if (((val ^ s_rx_buf[2]) & mask) == 0) {
            if (reg == 9) {
                do {
                    while (bus_ready_check((int)s_ctx) != 1) { }
                    s_tx_buf[0] = 0x9B;
                    s_tx_buf[1] = 1;
                    s_tx_buf[2] = 0xFF;
                    *s_tx_len = 4;
                    gpio_bit_write(0x50000000, 0x8000, 0);
                    if (smbus_transmit((int *)s_ctx, (int)s_tx_buf, (int)s_rx_buf, (int16_t)*s_tx_len) != 0) {
                        system_reset();
                    }
                    do { } while (bus_ready_check((int)s_ctx) != 1);
                    s_rx_buf[0] = s_tx_buf[0];
                    s_rx_buf[1] = s_tx_buf[1];
                    attempt++;
                    if (attempt > 9) system_reset();
                } while (crc8_verify((uint8_t *)s_rx_buf, 3) != s_rx_buf[3]);

                if ((val & mask) == (s_rx_buf[2] & mask & 3)) {
                    *s_ret_val = s_rx_buf[2] & 3;
                } else {
                    retry = true;
                }
            }
        } else {
            retry = true;
        }
    } while (retry);
}

/*
 * SMBus read — read 'count' bytes from FEDL5236 register 'addr'.
 * Returns 1 on success, 0 on persistent NACK.
 */
uint32_t smbus_read(uint8_t addr, uint8_t count)
{
    volatile uint8_t  * const s_ctx     = (volatile uint8_t  *)0x20002BA4;
    volatile uint8_t  * const s_status  = (volatile uint8_t  *)0x20002C80;
    volatile uint8_t  * const s_tx_buf  = (volatile uint8_t  *)0x20002B64;
    volatile uint32_t * const s_tx_len  = (volatile uint32_t *)0x20002B61;
    volatile uint8_t  * const s_rx_buf  = (volatile uint8_t  *)0x20002B84;
    uint8_t outer = 0;

    do {
        uint8_t inner = 0;
        while (bus_ready_check((int)s_ctx) != 1) {
            if ((*s_status & 1) != 0) {
                *s_status &= ~1U;
                inner++;
                if (inner > 9) return 0;
            }
        }

        s_tx_buf[0] = (addr << 1) | 0x81;
        s_tx_buf[1] = count;
        s_tx_buf[2] = 0xFF;
        *s_tx_len = count + 3;

        gpio_bit_write(0x50000000, 0x8000, 0);

        if (smbus_transmit((int *)s_ctx, (int)s_tx_buf, (int)s_rx_buf, (int16_t)*s_tx_len) != 0) {
            system_reset();
        }

        do { } while (bus_ready_check((int)s_ctx) != 1);

        s_rx_buf[0] = s_tx_buf[0];
        s_rx_buf[1] = s_tx_buf[1];
        outer++;
        if (outer > 9) { outer = 0; system_reset(); }
    } while (crc8_verify((uint8_t *)s_rx_buf, count + 2) != s_rx_buf[count + 2]);

    return 1;
}

/*
 * SMBus read no-ack — fire-and-forget write with no response check.
 */
void smbus_read_nack(uint8_t addr, uint8_t val)
{
    volatile uint8_t  * const s_ctx     = (volatile uint8_t  *)0x20002BA4;
    volatile uint8_t  * const s_status  = (volatile uint8_t  *)0x20002C80;
    volatile uint8_t  * const s_tx_buf  = (volatile uint8_t  *)0x20002B64;
    volatile uint32_t * const s_tx_len  = (volatile uint32_t *)0x20002B61;
    volatile uint8_t  * const s_rx_buf  = (volatile uint8_t  *)0x20002B84;
    uint8_t retries = 0;

    do {
        while (bus_ready_check((int)s_ctx) != 1) {
            if ((*s_status & 1) == 0) continue;
            *s_status &= ~1U;
            retries++;
            if (retries >= 10) return;
        }

        if (addr == 9) {
            *(volatile uint8_t *)0x20002C88 = val;
        }

        s_tx_buf[0] = (addr << 1) | 0x80;
        s_tx_buf[1] = val;
        s_tx_buf[2] = (uint8_t)crc8_for_smbus((uint8_t *)s_tx_buf, 2);
        *s_tx_len = 3;

        gpio_bit_write(0x50000000, 0x8000, 0);

        if (smbus_transmit((int *)s_ctx, (int)s_tx_buf, (int)s_rx_buf, (int16_t)*s_tx_len) != 0) {
            system_reset();
        }

        retries = 0;
        do {
            if (bus_ready_check((int)s_ctx) == 1) return;
            if ((*s_status & 1) != 0) {
                *s_status &= ~1U;
                retries++;
            }
        } while (retries < 10);
    } while (retries < 10);
}
