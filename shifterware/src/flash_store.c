/* flash_store.c — page-level program/erase for on-chip flash.
 *
 * MM32F031 flash is programmed 16 bits at a time after KEYR is unlocked
 * with the standard 0x45670123, 0xCDEF89AB sequence (RM §3.4.2). Erase
 * is per-page (1 KB). All operations block on FLASH->SR.BSY.
 *
 * Mix of OEM-confirmed and speculative; each function is labeled.
 * flash_store.c.bak holds the pre-decomp version. */

#include "flash_store.h"
#include "mm32f031.h"

/* Defined in main.c. Clears the shared per-task flag bytes; called at
 * the tail of `flash_settings_commit`. */
extern void state_flags_reset(void);

/* ---- OEM-confirmed ------------------------------------------------- */

/* OEM @ 0x0800471C (12 B). Unconditionally writes both keys; does not
 * check the LOCK bit first. */
void flash_unlock(void)
{
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
}

/* OEM @ 0x08004728 (14 B). */
void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK_Msk;
}

/* OEM @ 0x08004C4A (6 B). Write-1-to-clear bits in FLASH->SR
 * (PGERR / WRPRTERR / EOP). */
void flash_clear_status(uint32_t bits)
{
    FLASH->SR = bits;
}

/* OEM @ 0x08003812 (32 B). High-level single-page erase that owns the
 * unlock/lock cycle. The inner sequence's success indication is
 * dropped on the floor by the OEM — callers must read FLASH->SR if
 * they care about errors. */
void flash_erase_page(uint32_t page_addr)
{
    flash_unlock();
    flash_clear_status(FLASH_SR_PGERR_Msk
                     | FLASH_SR_WRPRTERR_Msk
                     | FLASH_SR_EOP_Msk);   /* 0x34 */
    flash_do_page_erase(page_addr);
    flash_clear_status(FLASH_SR_EOP_Msk);   /* 0x20 */
    flash_lock();
}

/* OEM @ 0x08003832 (32 B). Erase `n_pages` consecutive pages starting
 * at `base_addr`. Each call to flash_erase_page does its own
 * unlock/lock — inefficient under the OEM's API but matches the OEM. */
void flash_erase_pages(uint32_t base_addr, int n_pages)
{
    uint32_t addr = base_addr;
    for (int i = 0; i < n_pages; i++) {
        flash_erase_page(addr);
        addr += FLASH_PAGE_SIZE;
    }
}

/* OEM @ 0x08003898 (54 B). Program `n_hw` halfwords from `src` to
 * `dst`. Bracketed by the same unlock / clear-PGERR / .. / clear-EOP
 * / lock dance as `flash_erase_page`. Used by the cmd 0x82 OTA chunk
 * path. */
void flash_program_range(uint16_t *dst, const uint16_t *src, uint32_t n_hw)
{
    flash_unlock();
    flash_clear_status(FLASH_SR_PGERR_Msk
                     | FLASH_SR_WRPRTERR_Msk
                     | FLASH_SR_EOP_Msk);   /* 0x34 */
    for (uint32_t i = 0; i < n_hw; i++) {
        (void)flash_program_halfword((uint32_t)&dst[i], src[i]);
    }
    flash_clear_status(FLASH_SR_EOP_Msk);   /* 0x20 */
    flash_lock();
}

#define FLASH_WAIT_LIMIT    0x0FFFu  /* OEM-chosen poll budget */
#define FLASH_PG_WAIT_LIMIT 0x000Fu  /* shorter budget used per halfword program (OEM literal) */

/* OEM @ 0x08004736 (54 B). */
int flash_get_status(void)
{
    uint32_t sr = FLASH->SR;
    if ((sr & FLASH_SR_BSY_Msk)      != 0u) return FLASH_ST_BUSY;
    if ((sr & FLASH_SR_PGERR_Msk)    != 0u) return FLASH_ST_PGERR;
    if ((sr & FLASH_SR_WRPRTERR_Msk) != 0u) return FLASH_ST_WRPRTERR;
    return FLASH_ST_READY;
}

/* OEM @ 0x0800476C (26 B). Volatile busy-wait used between SR polls. */
static void flash_busy_step(void)
{
    volatile int i;
    i = 0;
    i = 0xFF;
    while (i != 0) {
        i = i - 1;
    }
}

/* OEM @ 0x08004786 (44 B). */
int flash_wait_status(int timeout)
{
    int status = flash_get_status();
    while (status == FLASH_ST_BUSY && timeout != 0) {
        flash_busy_step();
        status = flash_get_status();
        timeout = timeout - 1;
    }
    if (timeout == 0) {
        status = FLASH_ST_TIMEOUT;
    }
    return status;
}

/* OEM @ 0x080047B2 (72 B). The `status == BUSY` check on the second
 * wait is dead under the helper's actual return contract (it cannot
 * return BUSY); preserved here to match OEM bytes. */
int flash_do_page_erase(uint32_t page_addr)
{
    int status = flash_wait_status(FLASH_WAIT_LIMIT);
    if (status != FLASH_ST_READY) return status;

    FLASH->CR |= FLASH_CR_PER_Msk;
    FLASH->AR  = page_addr;
    FLASH->CR |= FLASH_CR_STRT_Msk;

    status = flash_wait_status(FLASH_WAIT_LIMIT);
    if (status == FLASH_ST_BUSY) return status;

    /* OEM clears PER with mask 0x1FFD (= bits [12:0] less bit 1).
     * The reserved upper bits are zero in the OEM image; using
     * ~FLASH_CR_PER_Msk would also work but emits different bytes. */
    FLASH->CR = FLASH->CR & 0x1FFDu;

    return status;
}

/* OEM @ 0x080049B2 (66 B). Program a single halfword. Caller is
 * expected to have already unlocked the flash. Mirrors the OEM's
 * approach to clearing PG: literal pool entry `0x1FFD` shared with the
 * page-erase helper, so the program path emits `mask = 0x1FFD + 1 =
 * 0x1FFE` (clears bit 0 only). The "skip clearing PG on BUSY" branch
 * preserves the OEM byte sequence even though `flash_wait_status`
 * cannot actually return BUSY (it converts the busy state to TIMEOUT
 * when the poll budget runs out). */
int flash_program_halfword(uint32_t addr, uint16_t value)
{
    int status = flash_wait_status(FLASH_PG_WAIT_LIMIT);
    if (status != FLASH_ST_READY) return status;

    FLASH->CR |= FLASH_CR_PG_Msk;
    *(volatile uint16_t *)addr = value;

    status = flash_wait_status(FLASH_PG_WAIT_LIMIT);
    if (status == FLASH_ST_BUSY) return status;

    FLASH->CR = FLASH->CR & 0x1FFEu;
    return status;
}

/* OEM @ 0x08003178 (110 B). Read-modify-write of one halfword inside
 * the settings page. The OEM emits this with a redundant second
 * `flash_unlock` between the read-back loop and the erase — kept here
 * to match. Each call performs a full page erase, so calling N times
 * in a row (as `flash_settings_commit` does) costs N erases. Wasteful
 * but matches the OEM byte sequence. */
void settings_set_halfword(uint32_t offset, uint16_t value)
{
    uint16_t buf[8];

    flash_unlock();
    for (uint32_t i = 0u; i < 16u; i = (uint32_t)(uint16_t)(i + 2u)) {
        buf[i >> 1] = *(volatile const uint16_t *)(FLASH_SETTINGS_PAGE + i);
    }
    buf[offset >> 1] = value;

    flash_unlock();
    flash_do_page_erase(FLASH_SETTINGS_PAGE);

    for (uint32_t i = 0u; i < 16u; i = (uint32_t)(uint16_t)(i + 2u)) {
        flash_program_halfword(FLASH_SETTINGS_PAGE + i, buf[i >> 1]);
    }
    flash_lock();
}

/* OEM @ 0x080031E6 (118 B). Persist the bus-writable shifter state to
 * the settings page. Layout (8 halfwords, low byte = data, high byte
 * = 0 on a fresh erase):
 *
 *     HW[0] = (int8_t)G_STATE_FC      — operating-mode state byte
 *     HW[1] = (uint8_t)(G_COUNTER >> 24)
 *     HW[2] = (uint8_t)(G_COUNTER >> 16)
 *     HW[3] = (uint8_t)(G_COUNTER >>  8)
 *     HW[4] = (uint8_t)(G_COUNTER >>  0)
 *     HW[5] = G_5C_REGS[0]
 *     HW[6] = G_5C_REGS[1]
 *     HW[7] = G_5C_REGS[2]
 *
 * The OEM reads `*(int32_t*)0x200000FC` and takes `% 256` (signed) — a
 * standard GCC pattern for `(int8_t)` truncation. For the documented
 * value range of G_STATE_FC (0..2) this is the same as the low byte;
 * we write it as a sign-extend cast to match OEM bytes for any value.
 */
void flash_settings_commit(void)
{
    const int32_t  state_fc = *(volatile const int32_t  *)0x200000FCu;
    const uint32_t counter  = *(volatile const uint32_t *)0x200000F8u;
    const volatile uint8_t *const regs = (const volatile uint8_t *)0x20000100u;

    settings_set_halfword(0x0u, (uint16_t)(int16_t)(int8_t)(state_fc & 0xFF));
    settings_set_halfword(0x2u, (uint16_t)(uint8_t)(counter >> 24));
    settings_set_halfword(0x4u, (uint16_t)(uint8_t)(counter >> 16));
    settings_set_halfword(0x6u, (uint16_t)(uint8_t)(counter >>  8));
    settings_set_halfword(0x8u, (uint16_t)(uint8_t)(counter      ));
    settings_set_halfword(0xAu, (uint16_t)regs[0]);
    settings_set_halfword(0xCu, (uint16_t)regs[1]);
    settings_set_halfword(0xEu, (uint16_t)regs[2]);

    *(volatile uint8_t *)0x2000013Cu = 0u;   /* G_5C_BUSY */
    state_flags_reset();
}

/* SRAM globals shared with main.c. Mirroring the existing
 * convention in this file (`extern void state_flags_reset(void)`),
 * raw pointers to the relevant slots rather than a shared header. */
#define G_STATE_FC_BYTE     (*(volatile uint8_t  *)0x200000FCu)
#define G_STATE_FC_WORD     (*(volatile uint32_t *)0x200000FCu)
#define G_COUNTER_BE        (*(volatile uint32_t *)0x200000F8u)
#define G_5C_REGS           ((volatile uint8_t  *)0x20000100u)

/* OEM @ 0x080040A8 (10 B). One-line halfword fetch from the settings
 * page. Standalone symbol because the OEM emits it as a real call,
 * not an inline — `settings_load` calls it 8 times. */
uint16_t settings_read_halfword(uint32_t offset)
{
    return *(volatile uint16_t *)(FLASH_SETTINGS_PAGE + offset);
}

/* OEM @ 0x080040B2 (126 B). Inverse of `flash_settings_commit`.
 * Reads halfword 0 first as a magic / first-boot probe: if it's
 * `0xFFFF` (blank/erased flash) then `G_STATE_FC` and `G_COUNTER` are
 * both reset to zero — the rest of the records are still read but
 * `G_COUNTER` is gated off by the `G_STATE_FC != 0` test at the end.
 * Otherwise halfword 0 is re-read into `G_STATE_FC` (low byte), and
 * halfwords 2/4/6/8 are reassembled big-endian into `G_COUNTER`.
 * Halfwords 10/12/14 always go to `G_5C_REGS[0..2]` regardless of
 * the magic check. Three slots are stored as 32-bit `str` even when
 * the destination is only a byte — the OEM C source presumably
 * declared them as `int`; we mirror that to stay byte-equivalent in
 * the store-width pattern (the upper 3 bytes are unused padding in
 * the SRAM layout). */
void settings_load(void)
{
    if (settings_read_halfword(0u) == 0xFFFFu) {
        G_STATE_FC_WORD = 0u;
    } else {
        G_STATE_FC_WORD = (uint32_t)settings_read_halfword(0u);
    }

    /* Read the 4 BE-byte halfwords up front (OEM keeps them live in
     * r3/r4/r5/r6 across the next group of calls). */
    const uint16_t b3 = settings_read_halfword(2u);
    const uint16_t b2 = settings_read_halfword(4u);
    const uint16_t b1 = settings_read_halfword(6u);
    const uint16_t b0 = settings_read_halfword(8u);

    G_5C_REGS[0] = (uint8_t)settings_read_halfword(10u);
    G_5C_REGS[1] = (uint8_t)settings_read_halfword(12u);
    G_5C_REGS[2] = (uint8_t)settings_read_halfword(14u);

    if (G_STATE_FC_BYTE == 0u) {
        G_COUNTER_BE = 0u;
    } else {
        G_COUNTER_BE = ((uint32_t)b3 << 24)
                     | ((uint32_t)b2 << 16)
                     | ((uint32_t)b1 << 8)
                     |  (uint32_t)b0;
    }
}

/* ---- Speculative (no OEM evidence yet) ----------------------------- */

bool flash_program_block(uint32_t addr, const void *data, size_t len_bytes)
{
    if ((addr & 0x1u) != 0u) return false;

    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0u;
    while (i + 1u < len_bytes) {
        const uint16_t hw = (uint16_t)(p[i] | ((uint16_t)p[i + 1u] << 8));
        if (flash_program_halfword(addr + (uint32_t)i, hw) != FLASH_ST_READY) return false;
        i += 2u;
    }
    if (i < len_bytes) {
        const uint16_t hw = (uint16_t)(p[i] | 0xFF00u);
        if (flash_program_halfword(addr + (uint32_t)i, hw) != FLASH_ST_READY) return false;
    }
    return true;
}
