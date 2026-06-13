#include <stdint.h>
#include <string.h>   /* memcpy (config-record bank copies) */

#include "crc.h"
#include "flash.h"
#include "log.h"

/* STM32F4 FLASH status register (RM0430 §3.7.5). flash_write clears the error
 * flags (write-1-to-clear) before programming. The OEM literal is 0x40023C00
 * (FLASH base) + 0x0C. */
#define FLASH_SR (*(volatile uint32_t *)0x40023C0Cu)
#define FLASH_CR (*(volatile uint32_t *)0x40023C10u)   /* +0x10: PSIZE[9:8], PG=bit0 */

/* --- HAL / runtime externs (recognised stock CubeF4, supplied later) ---
 * HAL_FLASH_Program          0x08027BE0  width-tagged program: TypeProgram 0/1/2/3
 *                                         = byte/half/word/dword. Data is a u64
 *                                         (ABI: value in r2, high word in r3).
 * HAL_FLASHEx_Erase          0x080235B4  CubeF4 HAL sector erase (EraseInit, &sector_error).
 * HAL_FLASH_Unlock           0x08027B14  KEY1/KEY2 unlock of FLASH_CR (idempotent).
 * FLASH_WaitForLastOperation 0x08027B80  poll FLASH_SR BSY with a tick timeout,
 *                                         clears the HAL handle ErrorCode (NOT a
 *                                         lock-acquire — the earlier name was wrong).
 * watchdog_kick              0x080314D8  refresh the watchdog (flash ops are slow). */
extern int      HAL_FLASH_Program(int type_program, uint32_t addr, uint32_t data_lo, uint32_t data_hi);
extern int      HAL_FLASHEx_Erase(void *erase_init, uint32_t *sector_error);
extern uint32_t HAL_FLASH_Unlock(void);
extern int      FLASH_WaitForLastOperation(int timeout_ticks);
extern void     watchdog_kick(void);

/* STM32 hardware CRC-32 accumulate over `len` words (OEM 0x08023234); the config
 * record reserves its last word for this checksum. The shared CRC handle is the
 * same crc_dev_t the rest of the firmware uses. */
extern uint32_t HAL_CRC_Accumulate(crc_dev_t *hcrc, uint32_t *buf, uint32_t len);
#define CRC_HANDLE  ((crc_dev_t *)0x20009D90u)

/* Map an absolute STM32F4(F413) flash address to its erase-sector index 0..15
 * (OEM flash_addr_to_sector, 0x0803CE14). Each test is an unsigned-underflow
 * range check (addr - sector_base) < sector_size. Sectors 0-3 = 16 KB,
 * 4 = 64 KB, 5+ = 128 KB; the 1 MB layout continues into the second bank. */
uint32_t flash_addr_to_sector(uint32_t flash_addr)
{
    if (flash_addr - 0x08000000U < 0x4000U)  return 0;   /* 16 KB sectors 0-3 */
    if (flash_addr - 0x08004000U < 0x4000U)  return 1;
    if (flash_addr - 0x08008000U < 0x4000U)  return 2;
    if (flash_addr - 0x0800C000U < 0x4000U)  return 3;
    if (flash_addr - 0x08010000U < 0x10000U) return 4;   /* 64 KB sector 4 */
    if (flash_addr - 0x08020000U < 0x20000U) return 5;   /* 128 KB sectors 5-7 */
    if (flash_addr - 0x08040000U < 0x20000U) return 6;
    if (flash_addr - 0x08060000U < 0x20000U) return 7;
    if (flash_addr - 0x08080000U < 0x20000U) return 8;   /* bank 2: sectors 8-11 */
    if (flash_addr - 0x080A0000U < 0x20000U) return 9;
    if (flash_addr - 0x080C0000U < 0x20000U) return 10;
    if (flash_addr - 0x080E0000U < 0x20000U) return 11;
    if (flash_addr - 0x08100000U < 0x20000U) return 12;
    if (flash_addr - 0x08120000U < 0x20000U) return 13;
    if (flash_addr - 0x08140000U < 0x20000U) return 14;
    return 15;
}

/* Program one 32-bit word (OEM flash_program_word, 0x08027A04): set PSIZE=x32 +
 * PG, then the store itself triggers the program. The dispatcher (flash_program)
 * is responsible for the unlock, the BSY poll, and clearing PG afterwards. */
void flash_program_word(volatile uint32_t *dst, uint32_t value)
{
    FLASH_CR &= ~0x300u;   /* clear PSIZE[9:8] */
    FLASH_CR |=  0x200u;   /* PSIZE = 0b10 (x32) */
    FLASH_CR |=  0x001u;   /* PG (program enable) */
    *dst = value;          /* the write programs the word */
}

/* Pre-erase/program housekeeping (OEM flash_unlock_and_clear_status, 0x0803CF1C):
 * unlock FLASH_CR via KEY1/KEY2, then clear the SR error flags. */
void flash_unlock_and_clear_status(void)
{
    HAL_FLASH_Unlock();
    FLASH_SR = 0xF3;       /* clear EOP/OPERR/WRPERR/PGAERR/PGPERR/PGSERR */
}

/* CubeF4 FLASH_EraseInitTypeDef (sector mode). */
struct flash_erase_init {
    uint32_t type_erase;     /* +0x00  0 = FLASH_TYPEERASE_SECTORS */
    uint32_t banks;          /* +0x04  (ignored for sector erase) */
    uint32_t sector;         /* +0x08 */
    uint32_t nb_sectors;     /* +0x0C */
    uint32_t voltage_range;  /* +0x10  2 = 2.7-3.6 V (word programming) */
};

static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }

int flash_erase(int addr, int len)
{
    flash_unlock_and_clear_status();

    int first = flash_addr_to_sector(addr);
    int last  = flash_addr_to_sector(addr + len);

    for (uint32_t i = 0; i < (uint32_t)(last - first); i++) {
        struct flash_erase_init ei;
        uint32_t sector_error;
        int rc;

        watchdog_kick();
        ei.type_erase    = 0;
        ei.banks         = 0;   /* OEM leaves this field unset; harmless for sectors */
        ei.sector        = (uint32_t)(first + (int)i);
        ei.nb_sectors    = 1;
        ei.voltage_range = 2;

        rc = HAL_FLASHEx_Erase(&ei, &sector_error);
        if (rc != 0) {
            g_log_func("Sector error %d\r\n", sector_error);  /* OEM str @0x08052F18 */
            return rc;
        }
    }
    return 0;
}

int flash_write(uint32_t addr, const uint32_t *data, int len)
{
    uint32_t end = addr + (uint32_t)len;

    irq_disable();
    FLASH_WaitForLastOperation(0xFFFF);   /* wait for any in-flight flash op */
    FLASH_SR = 0xF3;   /* clear PGSERR/PGPERR/PGAERR/WRPERR/EOP */

    while (addr < end) {
        watchdog_kick();
        int rc = HAL_FLASH_Program(2, addr, *data, 0);   /* program one 32-bit word */
        if (rc != 0) {
            /* Faithful OEM quirk: the error path returns with IRQs still
             * masked (no re-enable). */
            return rc;
        }
        addr += 4;
        data++;
    }

    irq_enable();
    return 0;
}

/* Validate a firmware OTA PACK image (OEM pack_validate, 0x0803CFD8).
 *
 * pack[] layout (LE words): [0]=magic 0xAA55AA55, [1]=version/type, [2]=stored
 * CRC-32, [3]=total length in bytes (< 0x40000, header included), [4..9]=rest
 * of the 10-word (0x28-byte) header, [10..]=image body.
 *
 * It resets the STM32 CRC unit, then runs the hardware CRC-32 over the header
 * (with the stored-CRC and length words masked to 0xFFFFFFFF) immediately
 * followed by the body (no reset between feeds), and compares against pack[2].
 * A normalised copy of the header is written into out_hdr as a side effect.
 * Returns 0 = CRC ok, 1 = CRC mismatch, 2 = bad magic or oversized length. */
uint32_t pack_validate(uint32_t *out_hdr, uint32_t *pack)
{
    crc_dev_t *dev = (crc_dev_t *)0x20009D90u;   /* CRC HAL handle */
    uint32_t crc;

    if (pack[0] != 0xAA55AA55u) {
        return 2;                                /* bad magic */
    }
    if (pack[3] >= 0x40000u) {
        return 2;                                /* length too large */
    }

    /* reset the CRC unit: CRC->CR bit0 (CR is at DR-base + 8). */
    ((volatile uint32_t *)dev->dr)[2] |= 1u;

    for (int i = 0; i < 10; i++) {               /* copy the 0x28-byte header */
        out_hdr[i] = pack[i];
    }

    /* seed the CRC over the header with the stored-CRC and length masked out */
    out_hdr[2] = 0xFFFFFFFFu;
    out_hdr[3] = 0xFFFFFFFFu;
    crc32_hw_feed(dev, out_hdr, 10);
    out_hdr[2] = pack[2];                         /* restore real fields */
    out_hdr[3] = pack[3];

    /* continue the same CRC over the body (header CRC state carries over) */
    crc = crc32_hw_feed(dev, pack + 10, (pack[3] - 0x28u) >> 2);
    out_hdr[1] = pack[1];

    return (pack[2] == crc) ? 0 : 1;
}

/* Dual-bank config persistence (OEM config_persist_dual_bank, 0x08031728): write
 * the 0xD0-byte config record ([a][b][c][d][0xC0 payload]) to both redundant banks
 * so a power loss mid-write can't brick the stored config. Bank A = 0x08008000
 * (sector 2), bank B = 0x0800C000 (sector 3), each a single 16 KB sector. The
 * load twin (flash_read_config_with_crc_restore) reads bank A and falls back to
 * bank B on a CRC failure. Returns the OR of the two per-bank status bytes
 * (0 = both ok; 1 erase / 2 write / 3 verify — see flash_config_bank_write). */
#define CONFIG_BANK_A_ADDR  0x08008000u   /* sector 2 */
#define CONFIG_BANK_B_ADDR  0x0800C000u   /* sector 3 */
#define CONFIG_BANK_SIZE    0x4000u       /* 16 KB */

uint8_t config_persist_dual_bank(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                 struct boot_cfg_block payload)
{
    uint8_t b1 = flash_config_bank_write(a, b, c, d, payload,
                                         CONFIG_BANK_A_ADDR, CONFIG_BANK_SIZE);
    uint8_t b2 = flash_config_bank_write(a, b, c, d, payload,
                                         CONFIG_BANK_B_ADDR, CONFIG_BANK_SIZE);
    return (uint8_t)(b1 | b2);
}

/* Commit one 0xD0-byte config record to a single flash bank (OEM 0x080316D0).
 * The record is [a][b][c][d] (the 4 register args = first 16 bytes) followed by
 * the 0xC0-byte payload (the by-value tail); its last word (offset 0xCC) is
 * reserved for the CRC. Sequence: erase -> CRC the first 0x33 words into the last
 * word -> program all 0xD0 bytes -> verify by re-CRC'ing 0x34 words straight from
 * flash (a self-checking CRC reads 0 when the stored checksum matches). Returns
 * 1 = erase fail, 2 = write fail, 3 = verify mismatch, 0 = ok. */
uint8_t flash_config_bank_write(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                struct boot_cfg_block payload,
                                uint32_t bank_dest, uint32_t size)
{
    union {
        uint32_t w[0x34];                                  /* 0xD0 bytes = 52 words */
        struct { uint32_t hdr[4]; struct boot_cfg_block body; } rec;
    } u;

    u.rec.hdr[0] = a;
    u.rec.hdr[1] = b;
    u.rec.hdr[2] = c;
    u.rec.hdr[3] = d;
    u.rec.body   = payload;

    if (flash_erase((int)bank_dest, (int)size) != 0) {
        return 1;
    }
    u.w[0x33] = HAL_CRC_Accumulate(CRC_HANDLE, u.w, 0x33);  /* checksum words 0..0x32 -> word 0x33 */
    if (flash_write(bank_dest, u.w, 0xD0) != 0) {
        return 2;
    }
    if (HAL_CRC_Accumulate(CRC_HANDLE, (uint32_t *)bank_dest, 0x34) != 0) {
        return 3;
    }
    return 0;
}

/* Load the config record from internal flash with CRC self-repair (OEM
 * flash_read_config_with_crc_restore, 0x08031784): copy bank A into `out` and
 * verify its 0x34-word self-checking CRC; on failure log + fall back to bank B,
 * and on a good bank B re-commit BOTH banks (healing the corrupt bank A) via
 * config_persist_dual_bank. Returns 0 if either bank was valid, 1 if both are
 * corrupt. `out` must be at least 0xD0 bytes. */
int flash_read_config_with_crc_restore(void *out)
{
    uint32_t *rec = (uint32_t *)out;

    memcpy(rec, (const void *)CONFIG_BANK_A_ADDR, 0xD0);
    if (HAL_CRC_Accumulate(CRC_HANDLE, rec, 0x34) == 0) {
        return 0;                                   /* bank A good */
    }

    g_log_func("  ERR:Read flash copy\r\n");
    memcpy(rec, (const void *)CONFIG_BANK_B_ADDR, 0xD0);
    if (HAL_CRC_Accumulate(CRC_HANDLE, rec, 0x34) != 0) {
        return 1;                                   /* both banks corrupt */
    }

    /* bank B recovered -> heal bank A by re-committing both banks (the 0xC0-byte
     * payload at rec+0x10 is passed by value, matching the OEM stack copy). */
    config_persist_dual_bank(rec[0], rec[1], rec[2], rec[3],
                             *(struct boot_cfg_block *)((uint8_t *)rec + 0x10));
    return 0;
}
