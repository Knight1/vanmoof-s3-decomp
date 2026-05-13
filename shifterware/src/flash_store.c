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

/* Status codes returned by flash_get_status() / flash_wait_status().
 * The first four mirror FLASH->SR bit semantics; TIMEOUT is synthetic
 * (only flash_wait_status returns it). */
#define FLASH_ST_BUSY       1
#define FLASH_ST_PGERR      2
#define FLASH_ST_WRPRTERR   3
#define FLASH_ST_READY      4
#define FLASH_ST_TIMEOUT    5

#define FLASH_WAIT_LIMIT    0x0FFFu  /* OEM-chosen poll budget */

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

/* ---- Speculative (no OEM evidence yet) ----------------------------- */

static void flash_wait_busy(void)
{
    while ((FLASH->SR & FLASH_SR_BSY_Msk) != 0u) {
        /* spin */
    }
}

bool flash_program_halfword(uint32_t addr, uint16_t value)
{
    if ((addr & 0x1u) != 0u) return false;

    flash_wait_busy();
    flash_clear_status(FLASH_SR_EOP_Msk
                     | FLASH_SR_PGERR_Msk
                     | FLASH_SR_WRPRTERR_Msk);

    FLASH->CR |= FLASH_CR_PG_Msk;
    *(volatile uint16_t *)addr = value;
    flash_wait_busy();

    const bool ok = (FLASH->SR & (FLASH_SR_PGERR_Msk | FLASH_SR_WRPRTERR_Msk)) == 0u;
    FLASH->CR &= ~FLASH_CR_PG_Msk;

    if (ok && *(volatile uint16_t *)addr != value) return false;
    return ok;
}

bool flash_program_block(uint32_t addr, const void *data, size_t len_bytes)
{
    if ((addr & 0x1u) != 0u) return false;

    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0u;
    while (i + 1u < len_bytes) {
        const uint16_t hw = (uint16_t)(p[i] | ((uint16_t)p[i + 1u] << 8));
        if (!flash_program_halfword(addr + (uint32_t)i, hw)) return false;
        i += 2u;
    }
    if (i < len_bytes) {
        const uint16_t hw = (uint16_t)(p[i] | 0xFF00u);
        if (!flash_program_halfword(addr + (uint32_t)i, hw)) return false;
    }
    return true;
}
