#include <stdint.h>
#include <string.h>

#include "stc3115.h"
#include "i2c.h"   /* HAL_I2C_Master_Transmit / _Receive, I2C_HandleTypeDef */

/* STC3115 fuel-gauge driver — transport, RAM/CRC and measurement layers. See
 * stc3115.h for the register map. The gauge shares I2C3 (handle 0x20009B04)
 * with the LIS3DH accelerometer; the 50 ms HAL timeout matches the OEM. */

#define STC_I2C   ((I2C_HandleTypeDef *)0x20009B04u)
#define STC_ADDR  0xE0u
#define STC_TMO   0x32u                              /* 50 ms */
#define STC_RAM   ((volatile uint8_t *)0x20006E80u)  /* g_stc3115_ram (16 B) */

/* ── transport ────────────────────────────────────────────────────────────── */
int stc3115_i2c_read(uint16_t len, uint8_t reg, uint8_t *out)
{
    uint8_t addr = reg;

    HAL_I2C_Master_Transmit(STC_I2C, STC_ADDR, &addr, 1, STC_TMO);
    return HAL_I2C_Master_Receive(STC_I2C, STC_ADDR, out, len, STC_TMO);
}

int stc3115_i2c_write(uint32_t len, uint8_t reg, const void *data)
{
    uint8_t buf[64];

    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return HAL_I2C_Master_Transmit(STC_I2C, STC_ADDR, buf, (uint16_t)(len + 1), STC_TMO);
}

uint32_t stc3115_read_reg(uint32_t reg)
{
    uint8_t b[8];

    if (stc3115_i2c_read(1, (uint8_t)reg, b) < 0) {
        return 0xFFFFFFFFu;
    }
    return b[0];
}

int stc3115_write_reg(uint32_t reg, uint8_t value)
{
    uint8_t b[8];

    b[0] = value;
    return stc3115_i2c_write(1, (uint8_t)reg, b);
}

int stc3115_read_word(uint8_t reg)
{
    uint8_t b[2];

    if (stc3115_i2c_read(2, reg, b) < 0) {
        return -1;
    }
    return (int)b[0] + (int)b[1] * 0x100;
}

void stc3115_write_word(uint8_t reg, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
    stc3115_i2c_write(2, reg, b);
}

int stc3115_read_block(uint8_t *dst, uint8_t reg, uint32_t len)
{
    return stc3115_i2c_read((uint16_t)len, reg, dst);
}

int stc3115_write_block(const void *src, uint8_t reg, uint32_t len)
{
    return stc3115_i2c_write(len, reg, src);
}

/* ── RAM working copy (gauge RAM regs 0x20..0x2F) ─────────────────────────── */
void stc3115_ram_read(uint8_t *dst)
{
    stc3115_read_block(dst, 0x20, 0x10);
}

int stc3115_ram_write(const void *src)
{
    return stc3115_write_block(src, 0x20, 0x10);
}

/* CRC-8, polynomial 0x07, MSB-first (the gauge-RAM integrity check). */
uint8_t stc3115_ram_crc8(const uint8_t *data, int len)
{
    uint32_t crc = 0;
    int i;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        int bit;
        for (bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if ((crc & 0x100) != 0) {
                crc ^= 7;
            }
        }
    }
    return (uint8_t)(crc & 0xff);
}

void stc3115_ram_update_crc(void)
{
    STC_RAM[0xf] = stc3115_ram_crc8((const uint8_t *)STC_RAM, 0xf);
}

void stc3115_ram_init(const uint8_t *cfg)
{
    volatile uint8_t *ram = STC_RAM;
    int i;

    for (i = 0; i < 0x10; i++) {
        ram[i] = 0;
    }
    *(volatile uint16_t *)ram         = 0x53A9;   /* RAM signature */
    *(volatile uint16_t *)(ram + 4)   = (uint16_t)(int16_t)(*(const int32_t *)(cfg + 0xc));
    *(volatile uint16_t *)(ram + 6)   = (uint16_t)(int16_t)(*(const int32_t *)(cfg + 0x10));
    stc3115_ram_update_crc();
}

/* ── conversion + measurement ─────────────────────────────────────────────── */
int stc3115_conv(int value, int scale)
{
    return ((value * scale >> 0xb) + 1) / 2;
}

uint32_t stc3115_check_id(void)
{
    if (stc3115_read_reg(0x18) == 0x14) {
        return (uint32_t)stc3115_read_word(0) & 0x7fff;
    }
    return 0xFFFFFFFEu;   /* -2 */
}

int stc3115_read_measurements(int *out)
{
    uint8_t b[16];
    int raw, v;

    if (stc3115_read_block(b, 0, 0xf) < 0) {
        return -1;
    }

    raw = (int)b[2] + (int)b[3] * 0x100;          /* SOC, regs 2-3 */
    out[1] = raw;
    out[2] = (raw * 10 + 0x100) >> 9;             /* SOC % */

    out[6] = (int)b[4] + (int)b[5] * 0x100;       /* coulomb counter, regs 4-5 */

    v = (int)b[6] + (int)b[7] * 0x100;            /* current, regs 6-7 (14-bit signed) */
    v &= 0x3fff;
    if ((v & 0x2000) != 0) {
        v -= 0x4000;
    }
    out[4] = stc3115_conv(v, 0x968);

    v = (int)b[8] + (int)b[9] * 0x100;            /* voltage, regs 8-9 (12-bit signed) */
    v &= 0xfff;
    if ((v & 0x800) != 0) {
        v -= 0x1000;
    }
    out[3] = stc3115_conv(v, 0x2333);

    v = (int)b[10];                               /* temperature, reg 10 (8-bit signed) */
    if (v > 0x7f) {
        v -= 0x100;
    }
    out[5] = v * 10;

    v = (int)b[13] + (int)b[14] * 0x100;          /* OCV, regs 13-14 (14-bit signed) */
    v &= 0x3fff;
    if ((v & 0x2000) != 0) {
        v -= 0x4000;
    }
    v = stc3115_conv(v, 0x2333);
    {
        int r = v + 2;                            /* rounding divide by 4 */
        if (r < 0) {
            r = v + 5;
        }
        out[7] = r >> 2;
    }
    return 0;
}

/* gas_gauge_reset (OEM 0x080396C4) — clear the two RAM-mirror fields (the coulomb
 * counter halfword and the CRC/valid byte at +9) at 0x20006E80, write the mirror
 * back to the gauge, and — if that succeeded — re-run the STC3115 by writing MODE
 * (reg 1) = 0x10. Returns the I2C status (0 = ok). */
int gas_gauge_reset(void)
{
    uint8_t *mirror = (uint8_t *)0x20006e80u;
    int rc;

    *(uint16_t *)mirror = 0;
    mirror[9] = 0;
    rc = stc3115_ram_write(mirror);
    if (rc == 0) {
        rc = stc3115_write_reg(1, 0x10);
    }
    return rc;
}
