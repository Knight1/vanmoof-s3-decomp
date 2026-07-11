#include <stdint.h>
#include <string.h>

#include "stc3115.h"
#include "i2c.h"   /* HAL_I2C_Master_Transmit / _Receive, I2C_HandleTypeDef */
#include "log.h"   /* g_log_func — stc3115_fuel_gauge_init error print */

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

/* ── config / startup / SOC-tracking algorithm ───────────────────────────────
 * The STMicro STC3115 driver top layer. `cfg` is the 0x30-byte config record
 * (int-indexed): [0] mode base, [1] CC_CNF scale, [2] Cnom, [3]/[4] R_int coeffs,
 * [5] capacity mAh, [6]/[7] alarm scaling, and a 16-byte OCV curve at byte +0x20.
 * `out`/battdata mirrors the STMicro GasGauge struct: [0] status word, [1] raw
 * SOC, [2] SOC (0.1%), [3] mV, [4] current, [5] temp, [6] counter, [8] present,
 * [9] remaining mAh, [10] remaining time. The 16-byte RAM mirror @0x20006E80
 * (sig 0x53A9, state byte +9: 'I'=init / 'R'=run / 'V'=restore) persists SOC
 * across sleep. */

/* stc3115_apply_config (OEM 0x080394DC) — push the config record to the gauge:
 * OCV curve -> regs 0x30.., CC_CNF (0x13), VM_CNF (0x14), the 0x15 scaling reg,
 * the RAM alarm words (regs 0xF/0x11), then CTRL and MODE = GG_RUN. */
void stc3115_apply_config(int *cfg)
{
    uint8_t *ram = (uint8_t *)0x20006e80u;

    stc3115_write_reg(0, 1);                          /* MODE: standby for config */
    stc3115_write_block(cfg + 8, 0x30, 0x10);         /* OCV curve -> regs 0x30.. */
    if (cfg[1] != 0) {
        stc3115_write_reg(0x13, (uint8_t)(cfg[1] << 1));        /* CC_CNF */
    }
    if (cfg[2] != 0) {
        int x  = cfg[2] << 9;
        /* VM_CNF: the OEM signed reciprocal-multiply (divisor ~9010, not a clean
         * power-of-ten, so kept in multiply form). */
        int vm = (int)((int64_t)0x745DC089 * x >> 0x2c) - (x >> 0x1f);
        stc3115_write_reg(0x14, (uint8_t)vm);                   /* VM_CNF */
    }
    if (cfg[6] != 0) {
        stc3115_write_reg(0x15, (uint8_t)((cfg[7] << 9) / (0x5e14 / cfg[6])));
    }
    if (*(uint16_t *)(ram + 4) != 0) {
        stc3115_write_word(0xf, *(uint16_t *)(ram + 4));        /* alarm SOC */
    }
    if (*(uint16_t *)(ram + 6) != 0) {
        stc3115_write_word(0x11, *(uint16_t *)(ram + 6));       /* alarm voltage */
    }
    stc3115_write_reg(1, 3);                          /* CTRL: GG_RST | IO0DATA */
    stc3115_write_reg(0, (uint8_t)((uint8_t)cfg[0] | 0x10));    /* MODE: GG_RUN */
}

/* stc3115_startup_from_ocv (OEM 0x08039580) — cold start: apply the config while
 * preserving REG_OCV (read it, apply, write it back) so the gauge seeds SOC from
 * the measured open-circuit voltage. Returns 0 on success, the check_id error else. */
int stc3115_startup_from_ocv(int *cfg)
{
    int rc = (int)stc3115_check_id();
    if (rc >= 0) {
        int ocv = stc3115_read_word(0xd);
        stc3115_apply_config(cfg);
        stc3115_write_word(0xd, (uint16_t)ocv);
        rc = 0;
    }
    return rc;
}

/* stc3115_startup_restore (OEM 0x080395A8) — warm start: apply the config, then
 * write the SOC saved in the RAM mirror (+2) back to REG_SOC (reg 2), continuing
 * the previous coulomb count. Returns 0 on success, the check_id error else. */
int stc3115_startup_restore(int *cfg)
{
    uint8_t *ram = (uint8_t *)0x20006e80u;
    int rc = (int)stc3115_check_id();
    if (rc >= 0) {
        stc3115_apply_config(cfg);
        stc3115_write_word(2, *(uint16_t *)(ram + 2));
        rc = 0;
    }
    return rc;
}

/* stc3115_init_device (OEM 0x080395D0) — build the default config record (Cnom
 * 3600, R_int 0xD4/0x6B, capacity 0x41A, zeroed OCV curve), then choose the
 * startup path from the RAM mirror: valid sig + CRC -> warm restore (or from-OCV
 * if CTRL says the gauge lost its count), else re-init RAM + from-OCV. Marks the
 * RAM state 'I' and writes it back. Returns the startup status. */
int stc3115_init_device(int *cfg, int *arg)
{
    uint8_t *ram = (uint8_t *)0x20006e80u;
    int ocv_curve[16];
    int rc;
    int i;

    memset(ocv_curve, 0, sizeof ocv_curve);
    cfg[0] = 0;
    cfg[6] = 10;
    cfg[3] = 0xd4;
    cfg[4] = 0x6b;
    for (i = 0; i < 16; i++) {
        if (ocv_curve[i] > 0x7f)  { ocv_curve[i] = 0x7f; }
        if (ocv_curve[i] < -0x7f) { ocv_curve[i] = -0x7f; }
        *((char *)cfg + i + 0x20) = (char)ocv_curve[i];
    }
    cfg[5] = 0x41a;
    cfg[7] = 0x34;
    cfg[1] = 10;
    cfg[2] = 0xe10;
    arg[8] = 1;

    stc3115_ram_read(ram);
    if (*(uint16_t *)ram == 0x53a9 && stc3115_ram_crc8(ram, 0x10) == 0) {
        if ((stc3115_read_reg(1) & 0x18) == 0) {
            rc = stc3115_startup_restore(cfg);
        } else {
            rc = stc3115_startup_from_ocv(cfg);
        }
    } else {
        stc3115_ram_init((uint8_t *)cfg);
        rc = stc3115_startup_from_ocv(cfg);
    }
    ram[9] = 0x49;                          /* 'I' */
    stc3115_ram_update_crc();
    stc3115_ram_write(ram);
    return rc;
}

/* stc3115_wake (OEM 0x080398CE) — clear MODE.VMODE (bit 0) to bring the gauge out
 * of the low-power voltage-only mode. Returns the I2C write status. */
int stc3115_wake(void)
{
    return stc3115_write_reg(0, (uint8_t)(stc3115_read_reg(0) & 0xfe));
}

/* stc3115_fuel_gauge_init (OEM 0x08037130) — boot-time gauge bring-up: run
 * stc3115_init_device against the STC context @0x20005DB4 (config @+0x38,
 * battdata @+8); on failure log "  ERR init STC3115", then seed the cached
 * reading (+0x10) with the 0xFFF "invalid" marker. */
void stc3115_fuel_gauge_init(void)
{
    uint8_t *ctx = (uint8_t *)0x20005db4u;

    if (stc3115_init_device((int *)(ctx + 0x38), (int *)(ctx + 8)) != 0) {
        g_log_func("  ERR init STC3115\r\n");
    }
    *(uint32_t *)(ctx + 0x10) = 0xfff;
}

/* stc_read (OEM 0x080396E4) — the periodic SOC-tracking task. Probe the gauge,
 * validate/rebuild the RAM mirror, run a startup if the gauge reset, read the
 * measurements, then in the 'R'(un) state derive SOC (with a 3.0-3.2 V linear
 * derate), remaining capacity (SOC*Cnom/1000) and a smoothed remaining-time
 * estimate, persisting SOC + the % byte to RAM. Returns 1 while running, 0
 * otherwise, -3 on a detected gauge reset, -4 on a measurement bus error, or the
 * negative check_id status. */
uint32_t stc_read(void *ctx, uint32_t *out_)
{
    uint8_t *cfg = (uint8_t *)ctx;
    int     *out = (int *)out_;
    uint8_t *ram = (uint8_t *)0x20006e80u;
    uint32_t rc;

    rc = stc3115_check_id();
    if ((int)rc < 0) {
        return rc;
    }
    out[0] = (int)rc;
    stc3115_ram_read(ram);
    if (*(uint16_t *)ram != 0x53a9 || stc3115_ram_crc8(ram, 0x10) != 0) {
        stc3115_ram_init(cfg);
        ram[9] = 0x49;                                  /* 'I' */
    }
    if ((out[0] & 0x800) == 0) {                        /* no POR since last read */
        if ((out[0] & 0x10) == 0) {                     /* gauge needs a startup */
            if (ram[9] == 0x56) {                       /* 'V' -> warm restore */
                stc3115_startup_restore((int *)cfg);
            } else {
                stc3115_startup_from_ocv((int *)cfg);
            }
            ram[9] = 0x49;                              /* 'I' */
        }
        if (stc3115_read_measurements(out) == 0) {
            if (ram[9] == 0x49 && out[6] > 4) {         /* enough charge moved -> run */
                ram[9] = 0x52;                          /* 'R' */
                out[8] = 1;
            }
            if (ram[9] == 0x52) {
                int v = out[3];                         /* voltage mV */
                if (v < 3000) {
                    out[2] = 0;
                } else if (v < 0xc80) {                 /* 3.0-3.2 V: linear derate */
                    out[2] = ((v - 3000) * out[2]) / 200;
                }
                out[9] = (out[2] * *(int *)(cfg + 0x14)) / 1000;   /* remaining mAh */
                if ((out[0] & 1) == 0) {                /* not voltage-only mode */
                    if (out[4] > 0x4b && out[2] > 0x3de) {          /* near-full, charging */
                        out[2] = 0x3de;
                        stc3115_write_word(2, 0xc600);
                    }
                    if (out[4] < 0) {                    /* discharging */
                        int t = ((out[9] / out[4]) * 0xf + out[10]) * 4;
                        out[10] = t / 5;                 /* smoothed remaining time */
                        if (out[10] < 0) {
                            out[10] = -1;
                        }
                    } else {
                        out[10] = -1;
                    }
                } else {
                    out[4] = 0;
                    out[10] = -1;
                }
                if (out[2] > 1000) { out[2] = 1000; }    /* clamp SOC 0..100.0% */
                if (out[2] < 0)    { out[2] = 0; }
            } else {
                out[9] = (out[2] * *(int *)(cfg + 0x14)) / 1000;
                out[4] = 0;
                out[5] = 0xfa;
                out[10] = -1;
            }
            *(int16_t *)(ram + 2) = (int16_t)out[1];     /* persist SOC */
            ram[8] = (uint8_t)((out[2] + 5) / 10);       /* SOC % byte */
            stc3115_ram_update_crc();
            stc3115_ram_write(ram);
            rc = (ram[9] == 0x52) ? 1u : 0u;
        } else {
            rc = 0xfffffffcu;                            /* -4: measurement bus error */
        }
    } else {
        out[8] = 0;
        gas_gauge_reset();
        rc = 0xfffffffdu;                                /* -3: gauge reset detected */
    }
    return rc;
}
