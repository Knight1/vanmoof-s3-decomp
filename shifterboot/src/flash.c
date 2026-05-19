/* flash.c — shifterboot's embedded-flash driver: unlock / lock,
 * status decode + poll, page erase (inner + wrapper + multi-page
 * iterator).
 *
 * All bodies translated directly from shifterboot's own
 * disassembly. Literals, bit positions and status codes are read
 * from the binary itself (see the per-function comments for the
 * OEM addresses and pool words used).
 */

#include "flash.h"
#include "image.h"

#include <stdint.h>

#define FLASH_BASE     (0x40022000u)

#define FLASH_KEYR     (*(volatile uint32_t *)(FLASH_BASE + 0x04u))
#define FLASH_SR       (*(volatile uint32_t *)(FLASH_BASE + 0x0Cu))
#define FLASH_CR       (*(volatile uint32_t *)(FLASH_BASE + 0x10u))
#define FLASH_AR       (*(volatile uint32_t *)(FLASH_BASE + 0x14u))

#define FLASH_KEY1     (0x45670123u)
#define FLASH_KEY2     (0xCDEF89ABu)

#define FLASH_CR_PER   (1u << 1)   /* page-erase enable */
#define FLASH_CR_STRT  (1u << 6)   /* start the configured op */
#define FLASH_CR_LOCK  (1u << 7)

/* Mask the OEM uses to clear `FLASH_CR.PER` while preserving the
 * other CR bits. `0x1FFD = ~0x0002 & 0x1FFF` — i.e. all bits up to
 * and including bit 12 except PER. The reserved upper bits are
 * zero in the OEM image, so `~FLASH_CR_PER` would be functionally
 * equivalent — but the literal-pool word at `0x080000A7C` encodes
 * `0x1FFD` specifically, and we mirror it. */
#define FLASH_CR_CLEAR_PER_MASK  0x1FFDu

/* Mask the OEM uses to clear `FLASH_CR.PG` while preserving the
 * other CR bits. `0x1FFE = (0x1FFD) + 1` — the OEM materialises this
 * by adding 1 to the same `0x1FFD` literal pool word the page-erase
 * path uses, saving a second literal slot. */
#define FLASH_CR_PG    (1u << 0)   /* program enable */
#define FLASH_CR_CLEAR_PG_MASK   0x1FFEu

/* OEM @ 0x08000946 (64 B). Inner halfword programmer. Sole caller
 * is `flash_program_range` (which precedes the call with unlock +
 * SR clear, the same recipe `flash_erase_page` uses for the inner
 * erase).
 *
 *   1. `flash_wait_status(15)` — short poll budget; programming a
 *      halfword is fast. If not READY, bail and return that status.
 *      The OEM materialises `15` as `movs r0, #0xF`.
 *   2. Set `FLASH->CR.PG` (bit 0 — `movs r1, #1`).
 *   3. `*(uint16_t *)addr = value` — the actual flash write.
 *   4. Poll again. If BUSY (= timeout under our enum mapping), bail
 *      without clearing PG — same dead-but-preserved branch as the
 *      erase path.
 *   5. Clear PG via `FLASH->CR &= 0x1FFE`. The OEM materialises
 *      `0x1FFE` as `0x1FFD + 1` (`adds r1, r1, #1`) so the literal
 *      pool word `0x1FFD` can be shared with `flash_do_page_erase`. */
int flash_program_halfword(uint32_t addr, uint16_t value)
{
    int status = flash_wait_status(FLASH_PROGRAM_WAIT_LIMIT);
    if (status != FLASH_ST_READY) {
        return status;
    }

    FLASH_CR = FLASH_CR | FLASH_CR_PG;
    *(volatile uint16_t *)addr = value;

    status = flash_wait_status(FLASH_PROGRAM_WAIT_LIMIT);
    if (status == FLASH_ST_BUSY) {
        return status;
    }

    FLASH_CR = FLASH_CR & FLASH_CR_CLEAR_PG_MASK;

    return status;
}

/* OEM @ 0x080014FE (54 B). Program `count` consecutive halfwords at
 * flash `dst` from the source halfword array `src`. Wraps:
 *   - `flash_unlock`
 *   - `flash_clear_status(PGERR | WRPRTERR | EOP)` (mask 0x34)
 *   - per-halfword loop calling `flash_program_halfword(dst + i*2,
 *     src[i])` with `i` `uxth`-clamped to 16 bits
 *   - `flash_clear_status(EOP)` (mask 0x20)
 *   - `flash_lock`
 *
 * The OEM discards each `flash_program_halfword` return value (it's
 * the caller's responsibility, if any, to detect partial-write
 * errors). Same partitioning as `flash_erase_page`. */
void flash_program_range(uint32_t dst, const uint16_t *src, uint16_t count)
{
    flash_unlock();
    flash_clear_status(FLASH_SR_PGERR_Msk
                       | FLASH_SR_WRPRTERR_Msk
                       | FLASH_SR_EOP_Msk);

    for (uint16_t i = 0u; i < count; i = (uint16_t)(i + 1u)) {
        (void)flash_program_halfword(dst, src[i]);
        dst += 2u;
    }

    flash_clear_status(FLASH_SR_EOP_Msk);
    flash_lock();
}

/* OEM @ 0x08000746 (72 B). Inner page-erase.
 *
 *   1. Poll `flash_wait_status(0xFFF)` to make sure the controller
 *      is idle. If the result isn't `4` (READY), bail and return
 *      that status. The OEM materialises the `4` constant via
 *      `cmp r4, #0x4; bne`.
 *   2. Set FLASH->CR.PER (bit 1 — `movs r1, #0x2`), write
 *      `FLASH->AR = page_addr`, then set FLASH->CR.STRT (bit 6 —
 *      `movs r1, #0x40`).
 *   3. Poll `flash_wait_status` again. If the helper returns `1`
 *      (BUSY), bail without clearing PER. This is a dead branch
 *      under the actual contract of `flash_wait_status` (which
 *      can never return BUSY — it converts BUSY-at-timeout to
 *      TIMEOUT) but the bytes are emitted by the OEM, so we keep
 *      it.
 *   4. Clear PER while preserving the rest of CR via
 *      `FLASH->CR &= 0x1FFD` (literal pool word at
 *      `0x080000A7C`). */
int flash_do_page_erase(uint32_t page_addr)
{
    int status = flash_wait_status(FLASH_WAIT_LIMIT);
    if (status != FLASH_ST_READY) {
        return status;
    }

    FLASH_CR = FLASH_CR | FLASH_CR_PER;
    FLASH_AR = page_addr;
    FLASH_CR = FLASH_CR | FLASH_CR_STRT;

    status = flash_wait_status(FLASH_WAIT_LIMIT);
    if (status == FLASH_ST_BUSY) {
        return status;
    }

    FLASH_CR = FLASH_CR & FLASH_CR_CLEAR_PER_MASK;

    return status;
}

/* OEM @ 0x08000138 (32 B). Iterates `flash_erase_page` across
 * `n_pages` consecutive 1 KB pages. The OEM uses `blt` (signed
 * less-than) for the loop test, so `n_pages <= 0` is a no-op —
 * we mirror that with a signed counter. The page-size constant
 * is materialised inside the loop body via `movs r0, #1; lsls
 * r0, #10` rather than hoisted to a precomputed register — a
 * -O0 artefact. Called 4× from `main` (`0x0800028E`, `0x080002C6`,
 * `0x080002F2`, `0x080003F8`). */
void flash_erase_pages(uint32_t base_addr, int n_pages)
{
    uint32_t addr = base_addr;
    for (int i = 0; i < n_pages; i++) {
        flash_erase_page(addr);
        addr += FLASH_PAGE_SIZE;
    }
}

/* OEM @ 0x080006CA (54 B). Decode the current `FLASH->SR` into a
 * `FLASH_ST_*` code. Priority order is BSY > PGERR > WRPRTERR >
 * READY (cascade of `cmp/bne` in the OEM). The OEM isolates BSY
 * (SR bit 0) via `lsls #0x1F; lsrs #0x1F` rather than a literal
 * mask — one fewer pool word. PGERR (bit 2) and WRPRTERR (bit 4)
 * use the obvious `ands` against the small `movs` literals `4`
 * and `0x10`. The function does three separate `ldr; ldr` pairs
 * to re-fetch `FLASH->SR` each branch — matches `-O0` shape and
 * leaves the read unhoisted in case a previous test cleared the
 * bit (though in practice SR bits are sticky until W1C). */
int flash_get_status(void)
{
    int status = FLASH_ST_READY;

    if ((FLASH_SR & 0x1u) != 0u) {
        status = FLASH_ST_BUSY;
    } else if ((FLASH_SR & 0x4u) != 0u) {
        status = FLASH_ST_PGERR;
    } else if ((FLASH_SR & 0x10u) != 0u) {
        status = FLASH_ST_WRPRTERR;
    } else {
        status = FLASH_ST_READY;
    }
    return status;
}

/* OEM @ 0x08000700 (26 B). Busy-spin used between status polls.
 * The OEM stores the counter in a stack slot and emits the source
 * pattern `volatile int i; i = 0; i = 0xFF;` — those two stores
 * (`movs r0,#0; str r0,[sp]; movs r0,#0xFF; str r0,[sp]`) only
 * make sense if `i` is `volatile`; without `volatile` the compiler
 * folds the first assignment away. */
void flash_busy_step(void)
{
    volatile int i;
    i = 0;
    i = 0xFF;
    while (i != 0) {
        i = i - 1;
    }
}

/* OEM @ 0x0800071A (44 B). Poll `flash_get_status` until it
 * returns something other than `1` (BUSY), or until the timeout
 * count is exhausted. The OEM unconditionally writes `5`
 * (TIMEOUT) into the status register if `timeout` reaches zero
 * — even if the loop happened to escape on the same iteration
 * with a non-BUSY status. We preserve that subtle overwrite to
 * match the OEM bytes. */
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

/* OEM @ 0x080016A6 (74 B). Copy one flash region to another, page
 * by page, via the SRAM scratch buffer at `0x200000F2`. Used by
 * `main` (called at `0x08000296`) to mirror slot 1 (`0x08001800`)
 * into slot 2 (`0x08004800`) once slot 1 has been validated and
 * slot 2 has been erased.
 *
 * The size is implicit — derived as `(dst - src) / 0x400`. The OEM
 * encodes the operation's size into the slot placement:
 *   slot 1 = 0x08001800..0x08004800  (12 KB)
 *   slot 2 = 0x08004800..0x08007800  (12 KB)
 * → `(0x08004800 - 0x08001800) / 0x400 = 12 pages`.
 *
 * Each iteration reads one page from `src` into the scratch via
 * `image_copy_halfwords` (which goes halfword-by-halfword to
 * tolerate misalignment) then writes the page back to flash at
 * `dst` via `flash_program_range`. The OEM materialises `512`
 * (halfwords per page) as `movs r2, #1; lsls r2, #9` and `0x400`
 * (page-size byte stride) as `movs r0, #1; lsls r0, #10`. */
#define FLASH_COPY_SCRATCH      ((uint16_t *)0x200000F2u)
#define HALFWORDS_PER_PAGE      (FLASH_PAGE_SIZE / 2u)  /* 512 */

void flash_copy_region(uint32_t src, uint32_t dst)
{
    const uint32_t n_pages = (dst - src) / FLASH_PAGE_SIZE;

    for (uint32_t i = 0u; i < n_pages; i++) {
        image_copy_halfwords((const uint16_t *)src, FLASH_COPY_SCRATCH,
                             HALFWORDS_PER_PAGE);
        flash_program_range(dst, FLASH_COPY_SCRATCH, HALFWORDS_PER_PAGE);
        src += FLASH_PAGE_SIZE;
        dst += FLASH_PAGE_SIZE;
    }
}

/* OEM @ 0x080006B0. The canonical STM32F0 / MM32F031 unlock
 * sequence — KEY1 then KEY2 to `FLASH->KEYR` (offset 0x04). After
 * this the LOCK bit in `FLASH->CR` clears and erase/program ops
 * become writable. */
void flash_unlock(void)
{
    FLASH_KEYR = FLASH_KEY1;
    FLASH_KEYR = FLASH_KEY2;
}

/* OEM @ 0x080006BC. Re-arm `FLASH->CR.LOCK`. The OEM emits a
 * read-modify-write rather than a single OR-equals because the
 * other CR bits (PG, PER, MER, STRT, etc.) may still hold state
 * that the caller wants preserved. */
void flash_lock(void)
{
    FLASH_CR = FLASH_CR | FLASH_CR_LOCK;
}

/* OEM @ 0x08000BDE. Six bytes of `*(FLASH + 0xC) = r0`. Used to
 * write-1-clear any combination of {PGERR, WRPRTERR, EOP} between
 * erase / program operations. BSY (bit 0) ignores writes so it
 * isn't disturbed. */
void flash_clear_status(uint32_t bits)
{
    FLASH_SR = bits;
}

/* OEM @ 0x08001534. Five-step page-erase wrapper:
 *   1. unlock
 *   2. SR clear: PGERR | WRPRTERR | EOP   (0x34 — the OEM materialises
 *      this as `movs r0, #0x34`)
 *   3. inner erase
 *   4. SR clear: EOP                       (0x20 — `movs r0, #0x20`)
 *   5. lock
 *
 * The OEM discards the inner erase's return value here (it pops
 * `{r4, pc}` with whatever the inner left in r0). We mirror that
 * by ignoring the return rather than propagating it — matches the
 * outer caller `FUN_08000138` which doesn't inspect the result
 * either. */
void flash_erase_page(uint32_t page_addr)
{
    flash_unlock();
    flash_clear_status(FLASH_SR_PGERR_Msk
                       | FLASH_SR_WRPRTERR_Msk
                       | FLASH_SR_EOP_Msk);
    (void)flash_do_page_erase(page_addr);
    flash_clear_status(FLASH_SR_EOP_Msk);
    flash_lock();
}
