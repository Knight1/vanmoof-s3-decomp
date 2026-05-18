#ifndef SHIFTER_FLASH_STORE_H
#define SHIFTER_FLASH_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FLASH_PAGE_SIZE      1024u

/* Persistent settings page used by `flash_settings_commit`. This is the
 * second-to-last 1 KB page in the 32 KB flash; the last page is
 * reserved for `nvm.c`'s calibration record. */
#define FLASH_SETTINGS_PAGE  0x08007800u

/* Status codes returned by flash_get_status / flash_wait_status /
 * flash_program_halfword. First four mirror FLASH->SR bit semantics;
 * TIMEOUT is synthetic (only flash_wait_status returns it). */
#define FLASH_ST_BUSY       1
#define FLASH_ST_PGERR      2
#define FLASH_ST_WRPRTERR   3
#define FLASH_ST_READY      4
#define FLASH_ST_TIMEOUT    5

/* OEM-confirmed (void where the OEM provides no return; success
 * checked by reading FLASH->SR otherwise). */
void flash_unlock(void);                          /* @ 0x0800471C */
void flash_lock(void);                            /* @ 0x08004728 */
void flash_clear_status(uint32_t bits);           /* @ 0x08004C4A */
void flash_erase_page(uint32_t page_addr);        /* @ 0x08003812 */
void flash_erase_pages(uint32_t base, int n_pages); /* @ 0x08003832 */

/* Lower-level helpers. Return codes: see FLASH_ST_* above. */
int  flash_get_status(void);                      /* @ 0x08004736 */
int  flash_wait_status(int timeout);              /* @ 0x08004786 */
int  flash_do_page_erase(uint32_t page_addr);     /* @ 0x080047B2 */
int  flash_program_halfword(uint32_t addr, uint16_t value); /* @ 0x080049B2 */

/* Update a single halfword at `offset` inside `FLASH_SETTINGS_PAGE`
 * (offset must be even, 0..14). Read-modify-writes the whole 8-halfword
 * record: load all 8 into a stack buffer, replace one, erase the page,
 * program all 8 back. */
void settings_set_halfword(uint32_t offset, uint16_t value); /* @ 0x08003178 */

/* Commit the bus-writable shifter settings to flash. Persists
 * `G_STATE_FC` (low byte), `G_COUNTER` (4 bytes, big-endian), and
 * `G_5C_REGS[0..2]` into the settings page as 8 halfwords (each storing
 * the data in the low byte, high byte zero). Clears the deferred-commit
 * latch `G_5C_BUSY` at the end and calls `state_flags_reset`. Triggered
 * by Modbus cmd 0x5C long-form and by main's idle-reset epilogue. */
void flash_settings_commit(void);                 /* @ 0x080031E6 */

/* Load the persisted shifter settings from `FLASH_SETTINGS_PAGE` into
 * their SRAM globals during boot. Restores `G_STATE_FC` (or zeros it
 * if the page is blank, i.e. halfword 0 == 0xFFFF), `G_COUNTER` (BE32
 * across 4 halfwords), and `G_5C_REGS[0..2]`. Inverse of
 * `flash_settings_commit`. Called once from main's boot prologue. */
void settings_load(void);                         /* @ 0x080040B2 */

/* Read one halfword (16 bits) from offset within `FLASH_SETTINGS_PAGE`.
 * The OEM exposes this as a one-instruction inline helper called by
 * `settings_load`; we materialise it as a separate symbol so the call
 * graph stays one-to-one with the OEM. */
uint16_t settings_read_halfword(uint32_t offset); /* @ 0x080040A8 */

/* Speculative. */
bool flash_program_block(uint32_t addr, const void *data, size_t len_bytes);

/* Program a halfword buffer to a flash region. Wraps the unlock / clear-
 * status / loop / clear-status / lock dance around per-halfword writes
 * via `flash_program_halfword`. Called only from the OTA chunk-staging
 * path in `modbus_dispatch.c`. */
void flash_program_range(uint16_t *dst, const uint16_t *src, uint32_t n_hw); /* @ 0x08003898 */

#endif /* SHIFTER_FLASH_STORE_H */
