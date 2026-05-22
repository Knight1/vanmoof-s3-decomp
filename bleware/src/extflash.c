/* extflash.c — external SPI NOR-flash driver primitives.
 *
 * Decoded so far: `extflash_erase_range` (OEM @ 0x00016A50). The other
 * helpers are forward-declared here and stubbed weak in hal_stubs.S
 * until they land in their own .c body.
 *
 * Flash device: ~64 Mb (8 MiB observed) SPI NOR. Sector size 4 KB; the
 * chip-info field at `g_extflash_state.chip_info->capacity` selects
 * between 3-byte and 4-byte address modes (threshold > 16 MiB).
 *
 * Layout of the singleton state at `g_extflash_state` (DAT_00016AFC):
 *   +0x04 chip_info_t *chip_info    (capacity, sector size, mfgr id, …)
 *   +0x0C Semaphore_t  bus_mutex    (TI-RTOS handle; one-at-a-time
 *                                    access to the SPI bus + CS line)
 *
 * Standard SPI NOR opcodes used by this driver:
 *   0x05  RDSR  — read status register (WIP bit 0 set ⇒ busy)
 *   0x06  WREN  — write enable (sets WEL latch)
 *   0x20  SE    — sector erase, 4 KB (3- or 4-byte addr depending on
 *                 chip capacity)
 *
 * The 0x05 opcode is sourced indirectly via `*DAT_000217A8`; 0x06 via
 * `*(DAT_000244A4 + 1)`. Both are byte fields of a per-chip command
 * table, so the driver is portable across SPI NOR vendors that vary
 * those opcodes. For this build they resolve to the standard values.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* TI-RTOS Semaphore_pend / _post via the ROM jump table. The
 * `0xFFFFFFFF` timeout argument is "block forever". */
extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);
extern void ti_semaphore_post(uint32_t handle);

/* SPI bus primitives. CS is on a GPIO managed by the IOC driver
 * (FUN_00022E08 toggles the line state). */
extern void extflash_cs_assert(void);    /* OEM 0x00026F94 */
extern void extflash_cs_deassert(void);  /* OEM 0x00026F84 */
extern int  extflash_spi_tx(const void *buf, uint32_t len);  /* OEM 0x00024448 */
extern int  extflash_spi_rx(void       *buf, uint32_t len);  /* OEM 0x00024418 */

/* Status-register poller. Loops sending RDSR (0x05) and reading one
 * byte back until WIP (bit 0) clears. Returns 0 on success and writes
 * the post-clear status byte to *out_status (if non-NULL); -2 on a
 * bus error. OEM @ 0x00021764. */
extern int  extflash_wait_wip_clear(uint8_t *out_status);

/* Sends a single-byte WREN (0x06) framed by CS assert/deassert.
 * Returns 0 on success, -3 on a bus error. OEM @ 0x00024478. */
extern int  extflash_write_enable(void);

/* Per-chip state. We only access the two fields touched by
 * `extflash_erase_range` — chip-info pointer and the bus semaphore. */
struct extflash_chip_info {
    uint32_t capacity;        /* bytes; > 0x01000000 ⇒ 4-byte addressing */
    /* ... other fields (page size, mfgr, …) not used here */
};

struct extflash_state {
    uint32_t                          _pad0;
    const struct extflash_chip_info  *chip_info;   /* +0x04 */
    uint32_t                          _pad8;
    uint32_t                          bus_mutex;   /* +0x0C, TI-RTOS handle */
};

extern struct extflash_state g_extflash_state;   /* DAT_00016AFC */

#define EXTFLASH_SECTOR_SIZE     0x1000u
#define EXTFLASH_SECTOR_MASK     0xFFFFF000u      /* DAT_00016B00 */
#define EXTFLASH_OP_SECTOR_ERASE 0x20u

/* Erase every 4 KB sector touched by [addr, addr+len). Aligns the
 * start address down to the sector boundary and erases the implied
 * range — partial-sector erases are not possible on SPI NOR.
 *
 * Returns 1 on success, 0 on any sub-step failure (semaphore acquire,
 * busy-wait, write-enable, or SPI transmit).
 *
 * OEM @ 0x00016A50. The undefined8 return packs `local_20` (the SPI
 * command buffer) into the upper word — that's a stack-layout
 * coincidence of the OEM compiler, not a real second return value, so
 * the canonical signature is plain `int`.
 */
int extflash_erase_range(uint32_t addr, uint32_t len)
{
    if (ti_semaphore_pend(g_extflash_state.bus_mutex, 0xFFFFFFFFu) == 0) {
        return 0;
    }

    uint32_t sector_addr = addr & EXTFLASH_SECTOR_MASK;
    /* round-up division: (last_byte - first_byte + 0xFFE) >> 12 */
    uint32_t nsectors = ((addr + len) - sector_addr + (EXTFLASH_SECTOR_SIZE - 2)) >> 12;

    int ok = 1;
    int use_4byte_addr = (g_extflash_state.chip_info->capacity > 0x01000000u);

    while (nsectors-- != 0) {
        if (extflash_wait_wip_clear(NULL) != 0) { ok = 0; break; }
        if (extflash_write_enable()        != 0) { ok = 0; break; }

        /* Assemble the SE command frame. Byte 0 is always 0x20; the
         * address follows MSB-first, 3 or 4 bytes depending on chip
         * capacity. */
        uint8_t  cmd[5];
        uint32_t cmd_len;
        cmd[0] = EXTFLASH_OP_SECTOR_ERASE;
        if (use_4byte_addr) {
            cmd[1] = (uint8_t)(sector_addr >> 24);
            cmd[2] = (uint8_t)(sector_addr >> 16);
            cmd[3] = (uint8_t)(sector_addr >>  8);
            cmd[4] = (uint8_t) sector_addr;
            cmd_len = 5;
        } else {
            cmd[1] = (uint8_t)(sector_addr >> 16);
            cmd[2] = (uint8_t)(sector_addr >>  8);
            cmd[3] = (uint8_t) sector_addr;
            cmd_len = 4;
        }

        extflash_cs_assert();
        int tx_rc = extflash_spi_tx(cmd, cmd_len);
        extflash_cs_deassert();
        if (tx_rc != 0) { ok = 0; break; }

        sector_addr += EXTFLASH_SECTOR_SIZE;
    }

    ti_semaphore_post(g_extflash_state.bus_mutex);
    return ok;
}
