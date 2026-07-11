#ifndef MAINWARE_STC3115_H
#define MAINWARE_STC3115_H

#include <stdint.h>

/* ST STC3115 LiPo fuel gauge (coulomb counter + OCV) on I2C3, 8-bit address
 * 0xE0. Register map: 0 MODE, 1 CTRL, 2-3 SOC, 4-5 COUNTER, 6-7 CURRENT,
 * 8-9 VOLTAGE, 10 TEMP, 13-14 OCV, 0x13 CC_CNF, 0x14 VM_CNF, 0x18 ID (=0x14),
 * 0x20.. RAM. A 16-byte working copy of the gauge RAM is mirrored in SRAM at
 * 0x20006E80 (signature 0x53A9 at [0], CRC-8 at [15]). This header covers the
 * transport / RAM-CRC / conversion / measurement layer plus the config, startup
 * and SOC-tracking (stc_read) top layer. */

/* Low-level I2C: write the register address then read `len` bytes / write
 * `len` data bytes after the register byte. Return is the HAL status of the
 * read (i2c_read); the write path returns the HAL transmit status. */
int  stc3115_i2c_read(uint16_t len, uint8_t reg, uint8_t *out);   /* OEM 0x080392C0 */
int  stc3115_i2c_write(uint32_t len, uint8_t reg, const void *data); /* OEM 0x08039448 */

/* Register accessors (read_reg returns 0xFFFFFFFF on bus error). */
uint32_t stc3115_read_reg(uint32_t reg);          /* OEM 0x080393DC */
int      stc3115_write_reg(uint32_t reg, uint8_t value); /* OEM 0x080394A6 */
int      stc3115_read_word(uint8_t reg);          /* OEM 0x080393FE  (-1 on error, else LE u16) */
void     stc3115_write_word(uint8_t reg, uint16_t value); /* OEM 0x080394BE  (LE) */
int      stc3115_read_block(uint8_t *dst, uint8_t reg, uint32_t len);   /* OEM 0x080392F4 */
int      stc3115_write_block(const void *src, uint8_t reg, uint32_t len); /* OEM 0x0803948C */

/* 16-byte RAM working copy (reg 0x20..0x2F) + its CRC-8 (poly 0x07). */
void    stc3115_ram_read(uint8_t *dst);           /* OEM 0x08039302 */
int     stc3115_ram_write(const void *src);       /* OEM 0x0803949A */
int     gas_gauge_reset(void);                    /* OEM 0x080396C4 */
uint8_t stc3115_ram_crc8(const uint8_t *data, int len); /* OEM 0x0803924C */
void    stc3115_ram_update_crc(void);             /* OEM 0x08039280 */
void    stc3115_ram_init(const uint8_t *cfg);     /* OEM 0x08039294 */

/* Fixed-point measurement conversion: ((value*scale)>>11 + 1) / 2. */
int stc3115_conv(int value, int scale);           /* OEM 0x0803923C */

/* Probe the part ID (reg 0x18 == 0x14): returns -2 on mismatch, else the
 * MODE/CTRL word masked with 0x7FFF. OEM 0x08039428. */
uint32_t stc3115_check_id(void);

/* Burst-read regs 0..14 and convert into the caller's measurement struct:
 * out[1]=raw SOC, out[2]=SOC%, out[3]=voltage, out[4]=current, out[5]=temp,
 * out[6]=counter, out[7]=OCV. Returns 0 on success, <0 on bus error.
 * OEM 0x0803930E. */
int stc3115_read_measurements(int *out);

/* ── config / startup / SOC-tracking top layer ─────────────────────────────── */

/* Push the config record (OCV curve, CC_CNF/VM_CNF, alarms) and set MODE=GG_RUN.
 * `cfg` is the int-indexed 0x30-byte config record. OEM 0x080394DC. */
void stc3115_apply_config(int *cfg);

/* Cold start (seed SOC from REG_OCV, 0x08039580) / warm start (restore the saved
 * SOC, 0x080395A8). Both return 0 on success or the negative check_id error. */
int stc3115_startup_from_ocv(int *cfg);
int stc3115_startup_restore(int *cfg);

/* Build the default config record and pick the startup path from the RAM mirror;
 * returns the startup status. OEM stc3115_init_device 0x080395D0. */
int stc3115_init_device(int *cfg, int *arg);

/* Clear MODE.VMODE (bit 0) to leave low-power voltage-only mode; returns the I2C
 * write status. OEM stc3115_wake 0x080398CE. */
int stc3115_wake(void);

/* Boot-time gauge bring-up (init_device against the STC context @0x20005DB4).
 * OEM stc3115_fuel_gauge_init 0x08037130. */
void stc3115_fuel_gauge_init(void);

/* Periodic SOC-tracking read: 1 while running, 0 otherwise, -3 on gauge reset,
 * -4 on measurement bus error, else the negative check_id status. OEM 0x080396E4. */
uint32_t stc_read(void *ctx, uint32_t *out);

#endif
