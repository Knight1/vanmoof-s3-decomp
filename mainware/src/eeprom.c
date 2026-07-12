#include <stdint.h>
#include <string.h>   /* memcpy */

#include "eeprom.h"
#include "systick.h"
#include "log.h"       /* g_log_func, log_print_timestamp_prefix */

/* =====================================================================
 * On-board EEPROM map — Atmel/ST AT24C, I2C3 device 0xA0, 128 bytes
 * (byte addresses 0x00..0x7F). The whole device is reached only through
 * the three primitives in this file. It holds exactly two persisted
 * objects plus one read-only device block:
 *
 *   0x00..0x3B  State record, copy A (primary)   ── 0x3C bytes = 15 u32
 *   0x3C..0x3F  unused (4 bytes)                     words: words 0..0xD
 *   0x40..0x7B  State record, copy B (mirror)     ── are data, word 0xE
 *   0x7C..0x7F  unused (4 bytes)                     (byte off 0x38) is a
 *   cmd 0xFA    6-byte lock/security "ID" block      HW CRC-32 over
 *               (device special read, not in the      words 0..0xD.
 *                0x00..0x7F array; = Security_GetLockState in fw 1.9.x)
 *
 * The state record is a verbatim image of the session_ctx block
 * ctx+0x310..+0x347 (record word n == ctx+0x310 + 4*n); the CRC occupies
 * the 15th word, i.e. it replaces what would be ctx+0x348.
 *
 *   word  off A/B   ctx      field
 *    0    00/40    +0x310    lock/alarm state (+0x310 bike/alarm state,
 *                            +0x312 remote-lock, +0x313 log-by-app)
 *    1    04/44    +0x314    shipping-mode / misc state (+0x317 alarm)
 *    2    08/48    +0x318    counter
 *    3    0C/4C    +0x31C    trip distance / odometer (tenths of a km,u32)
 *    4-13 10..37   +0x320..  runtime counters / flags (+0x344 wake count)
 *    14   38/78    (CRC)     HW CRC-32 over words 0..0xD
 *
 * Writer: save_state_record_to_eeprom (app.c) writes BOTH copies (0x00
 *         then 0x40), 5 ms + watchdog kick between.
 * Reader: eeprom_read_config_with_crc_fallback reads copy A, CRC-checks
 *         (stored word 0xE vs recomputed), and falls back to copy B on a
 *         mismatch; returns 1 only if BOTH copies fail CRC. Run once by
 *         mainware_boot_init_sequence at boot.
 * ID blk: eeprom_read_id_block issues the 0xFA command + 6-byte read,
 *         also probed once at boot (the bike lock/security state).
 *
 * Every offset above was derived from this 1.07.06 binary and agrees with
 * the independent field map in dev/vanmoof/vanmoof-tools/README.md (that
 * map is for 1.09.03; the finer per-byte names + version caveats live in
 * docs/hardware.md, "EEPROM map" → cross-reference).
 * ===================================================================== */

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

/* HW CRC-32 over a word buffer (crc.c 0x08023234); the CRC handle @ SRAM 0x20009D90. */
extern uint32_t HAL_CRC_Accumulate(void *hcrc, uint32_t *buf, uint32_t len);

/* eeprom_read_config_with_crc_fallback (OEM 0x0803E1A8) — read the 0x3C-byte state
 * record (14 data words + a HW CRC-32 word) into *out from copy A (offset 0); if the
 * CRC mismatches, log "Read EErom copy" and retry from copy B (offset 0x40). Returns
 * 0 if either copy verifies, 1 on an EEPROM read error or if both copies fail CRC. */
int eeprom_read_config_with_crc_fallback(void *out)
{
    uint32_t  buf[15];
    uint32_t *o = (uint32_t *)out;

    if (eeprom_read_bounded(0, (uint8_t *)buf, 0x3c) != 0) {
        return 1;
    }
    memcpy(out, buf, 0x3c);
    if (o[0xe] == HAL_CRC_Accumulate((void *)0x20009d90u, buf, 0xe)) {
        return 0;
    }
    log_print_timestamp_prefix();
    g_log_func("Read EErom copy\r\n");
    eeprom_read_bounded(0x40, (uint8_t *)buf, 0x3c);
    memcpy(out, buf, 0x3c);
    if (o[0xe] != HAL_CRC_Accumulate((void *)0x20009d90u, buf, 0xe)) {
        return 1;
    }
    return 0;
}
