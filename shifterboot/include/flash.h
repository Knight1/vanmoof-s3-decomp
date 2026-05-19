#ifndef SHIFTERBOOT_FLASH_H
#define SHIFTERBOOT_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MM32F031 embedded flash controller helpers, decomp'd from
 * shifterboot.bin. The family mirrors the one in shifterware
 * (see `shifterware/src/flash_store.c`) — both firmwares use
 * the same unlock/clear-status/inner-erase/clear-status/lock
 * recipe around `flash_do_page_erase`. */

/* Page size on MM32F031: 1 KB. */
#define FLASH_PAGE_SIZE        0x400u

/* FLASH->SR write-1-to-clear masks. */
#define FLASH_SR_PGERR_Msk     (1u << 2)
#define FLASH_SR_WRPRTERR_Msk  (1u << 4)
#define FLASH_SR_EOP_Msk       (1u << 5)

/* Status codes returned by `flash_get_status` / `flash_wait_status` /
 * `flash_do_page_erase`. All values are read directly out of
 * shifterboot's own disassembly:
 *   - `flash_get_status`  (`0x080006CA`) loads `r0` with the literal
 *     `4` then overwrites it to `1`, `2`, or `3` when the matching
 *     `FLASH->SR` bit is set (BSY / PGERR / WRPRTERR respectively).
 *   - `flash_wait_status` (`0x0800071A`) compares the loop result
 *     against `1` (BUSY) and writes the literal `5` (TIMEOUT) on
 *     poll-budget exhaustion.
 *   - `flash_do_page_erase` (`0x08000746`) gates the erase on
 *     `status == 4` (READY) and bails on `status == 1` (BUSY).
 */
#define FLASH_ST_BUSY       1
#define FLASH_ST_PGERR      2
#define FLASH_ST_WRPRTERR   3
#define FLASH_ST_READY      4
#define FLASH_ST_TIMEOUT    5

/* Poll budget passed to `flash_wait_status` from the erase / program
 * paths. The OEM materialises 4095 (`0x00000FFF`) via a literal-pool
 * word at `0x080000A78`. */
#define FLASH_WAIT_LIMIT    0x00000FFF

/* Unlock the flash controller for erase / program. Writes the
 * canonical KEY1 (`0x45670123`) + KEY2 (`0xCDEF89AB`) sequence to
 * `FLASH->KEYR`. */
void flash_unlock(void);

/* Re-arm the flash LOCK bit (`FLASH->CR |= 1<<7`). Pair with
 * `flash_unlock`. */
void flash_lock(void);

/* Write `bits` to `FLASH->SR`. The W1C bits in `FLASH->SR`
 * (PGERR, WRPRTERR, EOP) are cleared by writing `1` to them; the
 * BSY bit ignores writes. Used to ack post-erase / post-program
 * status. */
void flash_clear_status(uint32_t bits);

/* Erase `n_pages` consecutive 1 KB flash pages starting at
 * `base_addr`. Walks `flash_erase_page(base_addr + i*0x400)` in a
 * signed counted loop (so `n_pages <= 0` is a no-op). The OEM
 * unlocks/locks the flash on every page rather than once at the
 * top — inefficient but matches the OEM byte sequence. */
void flash_erase_pages(uint32_t base_addr, int n_pages);

/* Erase one 1 KB flash page starting at `page_addr`. Wraps:
 *   - flash_unlock
 *   - clear PGERR | WRPRTERR | EOP
 *   - inner page-erase (which itself sets PER + AR + STRT and
 *     waits for BSY to drop / EOP to set)
 *   - clear EOP (post-erase)
 *   - flash_lock
 * `page_addr` must be page-aligned (no error check; the OEM
 * relies on the caller).
 *
 * Returns the inner erase's status code (see `flash_get_status`
 * once that is decomp'd — currently the inner function lives in
 * `FUN_08000746` as `pending`). */
void flash_erase_page(uint32_t page_addr);

/* Inner page-erase. Caller (currently only `flash_erase_page`) is
 * expected to have already unlocked the flash and cleared the
 * sticky SR error bits. Returns one of `FLASH_ST_READY` (success),
 * `FLASH_ST_PGERR` / `FLASH_ST_WRPRTERR` (hardware reported an
 * error), or `FLASH_ST_TIMEOUT` (poll budget exhausted). Cannot
 * actually return `FLASH_ST_BUSY` — `flash_wait_status` converts
 * the busy-at-timeout case to TIMEOUT. */
int flash_do_page_erase(uint32_t page_addr);

/* Decode the current `FLASH->SR` into a `FLASH_ST_*` code. Priority
 * (matches the cascaded `cmp ... bne` chain at OEM @ 0x080006CA):
 * BSY > PGERR > WRPRTERR > READY. The function isolates BSY via
 * `lsls #0x1F; lsrs #0x1F` (shift bit 0 into bit 31 and back) rather
 * than masking — same effect, one fewer literal. */
int flash_get_status(void);

/* Busy-spin used between status polls. The OEM stores the counter
 * in a stack slot and the two initial stores (`*sp = 0; *sp = 0xFF`)
 * give the source-level pattern `volatile int i; i = 0; i = 0xFF;`
 * — without `volatile` the compiler would fold the assignment to a
 * single store. */
void flash_busy_step(void);

/* Poll `flash_get_status` until it returns something other than
 * BUSY, or until `timeout` count is exhausted. Returns either the
 * final non-BUSY status or `FLASH_ST_TIMEOUT` (synthetic). */
int flash_wait_status(int timeout);

/* Program a single halfword at flash `addr` with value `value`. Caller
 * is expected to have already unlocked the flash and cleared the
 * sticky SR error bits. Returns the same `FLASH_ST_*` enumeration as
 * `flash_do_page_erase`. Internally uses a much shorter poll budget
 * (`FLASH_PROGRAM_WAIT_LIMIT` = 15) than the erase path; programming
 * a single halfword is fast. */
int flash_program_halfword(uint32_t addr, uint16_t value);

/* Program `count` consecutive halfwords at flash `dst` from the
 * source halfword array `src`. The wrapper:
 *   - flash_unlock
 *   - flash_clear_status(PGERR | WRPRTERR | EOP)
 *   - for i in 0..count-1: flash_program_halfword(dst + i*2, src[i])
 *   - flash_clear_status(EOP)
 *   - flash_lock
 * Used by the OTA chain (FUN_080016A6 = "halfword copier") and by
 * FUN_08001658 (both still pending decomp). */
void flash_program_range(uint32_t dst, const uint16_t *src, uint16_t count);

/* Copy a flash region of size `(dst - src)` bytes from `src` to `dst`,
 * page by page (1 KB pages), routed through an SRAM scratch buffer
 * at `0x200000F2`. The size is **implicit**: the OEM's caller (main's
 * image-sync path) relies on the two image slots being placed
 * contiguously in flash, so `dst - src` equals the size of the source
 * region. For the OEM layout — slot 1 at `0x08001800`, slot 2 at
 * `0x08004800` — this gives `n_pages = 12` (= 12 KB).
 *
 * Caller is responsible for ensuring `dst` is erased first
 * (`flash_erase_pages` over the destination range). */
void flash_copy_region(uint32_t src, uint32_t dst);

/* Poll budget used by `flash_program_halfword`. The OEM materialises
 * `15` as `movs r0, #0xF`. */
#define FLASH_PROGRAM_WAIT_LIMIT  15

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_FLASH_H */
