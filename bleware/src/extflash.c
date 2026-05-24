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
#include <string.h>

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
    uint32_t    capacity;     /* +0x00 bytes; > 0x01000000 ⇒ 4-byte addressing */
    uint8_t     jedec_mfgr;   /* +0x04 REMS byte 1 (e.g. 0xC2 Macronix, 0xEF Winbond) */
    uint8_t     jedec_dev;    /* +0x05 REMS byte 2 ("capacity-3" device id) */
    uint16_t    _pad6;        /* +0x06 always 0x0000 */
    const char *part_name;    /* +0x08 → 12-byte slot in g_extflash_part_name_table */
    uint32_t    sector_size;  /* +0x0C bytes (always 0x1000 in observed entries) */
};

/* Supported parts (from the embedded vendor table at flash 0x0002A46C):
 *
 *   capacity      mfgr/dev   part
 *   0x04000000    C2 / 19    MX25L51245G   (Macronix 512 Mb)
 *   0x00200000    C2 / 15    MX25R1635F    (Macronix 16 Mb ULP)
 *   0x00100000    C2 / 14    MX25R8035F    (Macronix 8 Mb ULP)
 *   0x00080000    EF / 12    W25X40CL      (Winbond 4 Mb)
 *   0x00040000    EF / 11    W25X20BV      (Winbond 2 Mb)
 *
 * Only the MX25L51245G part requires 4-byte addressing — the other four
 * fit inside the 16 MiB / 24-bit boundary. The S3 production bike ships
 * with the 64 MiB MX25L51245G (consistent with the 0x80000-step OAD
 * slot map and the 0x03FDD000 log region). The smaller parts are
 * legacy / dev-rig fall-backs that this firmware still supports.
 */

/* Top of the chip-info table at flash 0x0002A450. Bytes 0..27 are a
 * driver-level header; the per-vendor entries (the array we search
 * against the REMS read) start at +0x1C and are 16 bytes each. The
 * array is terminated by a row of zeros (`capacity == 0`).
 *
 *   +0x00 u8   enter_4byte_mode_cmd  (0xB7  EN4B)
 *   +0x01 u8   rdsr_cmd              (0x05  RDSR) ← sourced by wait_wip_clear
 *   +0x02 u8   wren_cmd              (0x06  WREN) ← sourced by write_enable
 *   +0x03 u8   wrsr_cmd              (0x01  WRSR)
 *   +0x04 u8   pad
 *   +0x05..+0x08  4-byte REMS cmd burst (90 FF FF 00 — opcode + dummy
 *                                         addr selecting which byte
 *                                         comes out first)
 *   +0x09..+0x0B  pad
 *   +0x0C..+0x1B  16-byte SPI driver init params (TI-SDK SPI_Params
 *                                                 template, copied
 *                                                 verbatim into the
 *                                                 stack frame by
 *                                                 extflash_open)
 *   +0x1C..      vendor entry array (struct extflash_chip_info[]) */
extern const struct extflash_chip_info g_extflash_vendor_table[];     /* DAT_0001A3A8 → 0x0002A46C */
extern const uint8_t                   g_extflash_driver_header[];    /* 0x0002A450 */
extern const char                      g_extflash_part_name_table[][12];

/* `extflash_identify_chip` — REMS-based JEDEC ID probe.
 *
 * Sends the 4-byte REMS burst from the driver header, reads 2 bytes
 * back (mfgr, dev), and linearly searches `g_extflash_vendor_table`
 * for a match. On hit, writes the entry pointer into
 * `g_extflash_state.chip_info` and (if the part is > 16 MiB) sends
 * the 0xB7 EN4B byte to switch the chip into 4-byte addressing mode
 * so subsequent READ/PP/SE opcodes can use 32-bit addresses.
 *
 * Returns 1 on match (chip_info is now valid), 0 otherwise.
 *
 * OEM @ 0x0001A328.
 */
extern int extflash_identify_chip(void);

struct extflash_state {
    uint32_t                          _pad0;
    const struct extflash_chip_info  *chip_info;   /* +0x04 */
    uint32_t                          _pad8;
    uint32_t                          bus_mutex;   /* +0x0C, TI-RTOS handle */
};

extern struct extflash_state g_extflash_state;   /* DAT_00016AFC */

#define EXTFLASH_SECTOR_SIZE     0x1000u
#define EXTFLASH_SECTOR_MASK     0xFFFFF000u      /* DAT_00016B00 */
#define EXTFLASH_PAGE_SIZE       0x100u
#define EXTFLASH_OP_READ         0x03u
#define EXTFLASH_OP_PAGE_PROGRAM 0x02u
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

/* Standard READ (0x03) — single SPI burst, no dummy cycle, no per-page
 * splitting (READ keeps incrementing the internal address as long as
 * CS stays asserted).
 *
 * Returns 1 on success, 0 on any sub-step failure.
 *
 * OEM @ 0x0001C5A4.
 */
int extflash_read(uint32_t addr, uint32_t len, void *dst)
{
    if (ti_semaphore_pend(g_extflash_state.bus_mutex, 0xFFFFFFFFu) == 0) {
        return 0;
    }
    if (extflash_wait_wip_clear(NULL) != 0) {
        ti_semaphore_post(g_extflash_state.bus_mutex);
        return 0;
    }

    uint8_t  cmd[5];
    uint32_t cmd_len;
    int use_4byte_addr = (g_extflash_state.chip_info->capacity > 0x01000000u);
    cmd[0] = EXTFLASH_OP_READ;
    if (use_4byte_addr) {
        cmd[1] = (uint8_t)(addr >> 24);
        cmd[2] = (uint8_t)(addr >> 16);
        cmd[3] = (uint8_t)(addr >>  8);
        cmd[4] = (uint8_t) addr;
        cmd_len = 5;
    } else {
        cmd[1] = (uint8_t)(addr >> 16);
        cmd[2] = (uint8_t)(addr >>  8);
        cmd[3] = (uint8_t) addr;
        cmd_len = 4;
    }

    extflash_cs_assert();
    int rc = extflash_spi_tx(cmd, cmd_len);
    if (rc == 0) {
        rc = extflash_spi_rx(dst, len);
    }
    extflash_cs_deassert();
    ti_semaphore_post(g_extflash_state.bus_mutex);
    return (rc == 0);
}

/* Returns the chip-info pointer stored on the singleton state struct
 * (populated at driver init from the JEDEC ID read). OEM @ 0x000273D0
 * — just `return g_extflash_state.chip_info`. */
const struct extflash_chip_info *extflash_get_chip_info(void)
{
    return g_extflash_state.chip_info;
}

/* Page-Program (PP, 0x02) one or more 256-byte pages. The destination
 * range [addr, addr+len) must already be erased — PP can only flip
 * bits 1→0. Splits at 256-byte page boundaries since PP wraps inside a
 * page and does NOT advance to the next.
 *
 * Returns 1 on success, 0 on any sub-step failure.
 *
 * OEM @ 0x00015B9C.
 */
int extflash_write(uint32_t addr, uint32_t len, const void *src)
{
    if (ti_semaphore_pend(g_extflash_state.bus_mutex, 0xFFFFFFFFu) == 0) {
        return 0;
    }

    const uint8_t *p = (const uint8_t *)src;
    int ok = 1;
    int use_4byte_addr = (g_extflash_state.chip_info->capacity > 0x01000000u);

    while (len != 0) {
        if (extflash_wait_wip_clear(NULL) != 0) { ok = 0; break; }
        if (extflash_write_enable()        != 0) { ok = 0; break; }

        /* Clamp the chunk to the current 256-byte page tail. */
        uint32_t chunk = EXTFLASH_PAGE_SIZE - (addr & 0xFF);
        if (chunk > len) {
            chunk = len;
        }

        uint8_t  cmd[5];
        uint32_t cmd_len;
        cmd[0] = EXTFLASH_OP_PAGE_PROGRAM;
        if (use_4byte_addr) {
            cmd[1] = (uint8_t)(addr >> 24);
            cmd[2] = (uint8_t)(addr >> 16);
            cmd[3] = (uint8_t)(addr >>  8);
            cmd[4] = (uint8_t) addr;
            cmd_len = 5;
        } else {
            cmd[1] = (uint8_t)(addr >> 16);
            cmd[2] = (uint8_t)(addr >>  8);
            cmd[3] = (uint8_t) addr;
            cmd_len = 4;
        }

        extflash_cs_assert();
        int rc = extflash_spi_tx(cmd, cmd_len);
        if (rc == 0) {
            rc = extflash_spi_tx(p, chunk);
        }
        extflash_cs_deassert();
        if (rc != 0) { ok = 0; break; }

        addr += chunk;
        p    += chunk;
        len  -= chunk;
    }

    ti_semaphore_post(g_extflash_state.bus_mutex);
    return ok;
}

/* ---- GPIO / CS helpers — IOC pin toggling -------------------------- */

/* IOC GPIO writer — sets the DIO pin `pin` to `value` (0=low, 1=high).
 * Called by extflash CS helpers AND ble_activity_led_pulse in ssp.c.
 * OEM @ 0x00022E08 (46 B). */
int gpio_write(const void *gpio_ctx, uint32_t pin, int value)
{
    extern uint32_t g_gpio_max_pin;
    extern uint32_t g_gpio_base_table[];
    extern uint32_t g_gpio_dout_base;
    if (pin > g_gpio_max_pin) return 2;
    if ((uint32_t)gpio_ctx != g_gpio_base_table[pin]) return 2;
    ((volatile uint8_t *)g_gpio_dout_base)[pin - 0xE0u] = (value != 0);
    return 0;
}

void extflash_cs_assert(void)
{
    extern void *g_extflash_gpio_ctx;
    gpio_write(g_extflash_gpio_ctx, 4, 0);
}

void extflash_cs_deassert(void)
{
    extern void *g_extflash_gpio_ctx;
    gpio_write(g_extflash_gpio_ctx, 4, 1);
}

int extflash_wait_wip_clear(uint8_t *out_status)
{
    extern uint8_t g_extflash_rdsr_cmd;
    uint8_t status;
    int rc;
    do {
        extflash_cs_assert();
        rc = extflash_spi_tx(&g_extflash_rdsr_cmd, 1);
        if (rc != 0) { extflash_cs_deassert(); return -2; }
        rc = extflash_spi_rx(&status, 1);
        extflash_cs_deassert();
        if (rc != 0) return -2;
    } while (status & 1u);
    if (out_status != NULL) *out_status = status;
    return 0;
}

int extflash_write_enable(void)
{
    extern uint8_t g_extflash_wren_cmd;
    extflash_cs_assert();
    int rc = extflash_spi_tx(&g_extflash_wren_cmd, 1);
    extflash_cs_deassert();
    return (rc == 0) ? 0 : -3;
}

/* Simple backoff — if retry counter is zero, sleep 2 ms and return
 * the counter; otherwise returns -1 (no more retries). OEM @ 0x00025B84. */
int extflash_retry_backoff(int retry)
{
    extern void FUN_000274f2(int ms);   /* clock/sleep wrapper */
    if (retry == 0) {
        FUN_000274f2(2);  /* sleep 2 ms */
        return retry;
    }
    return -1;
}

/* Close the external flash driver — releases the SPI bus handle.
 * OEM @ 0x0002758E (6 B, thunk). */
void extflash_close(void)
{
    /* The OEM body is a single tail-call to a ROM SPI-closing routine.
     * Stubbed here; the real implementation calls TI-SDK SPI_close. */
    extern void thunk_EXT_FUN_1002d420(void);
    thunk_EXT_FUN_1002d420();
}

/* ---- SPI TX/RX — driver wrapper calls ------------------------------- */

/* TI SPI driver transfer primitive (ROM/thunk). OEM @ 0x000274E8. */
extern int FUN_000274e8(void *handle, void *xfer);

/* SPI transmit. Builds a {len, buf} struct on the stack, calls the
 * TI SPI driver via `FUN_000274e8(handle, &struct)`. Returns 0 on
 * success, -1 on failure. OEM @ 0x00024448 (42 B). */
int extflash_spi_tx(const void *buf, uint32_t len)
{
    extern void *g_extflash_spi_handle;
    extern int   FUN_000274e8(void *handle, void *xfer);

    struct { uint32_t len; const void *buf; uint32_t mode; } xfer;
    xfer.mode = 0;
    xfer.len  = len;
    xfer.buf  = buf;

    return (FUN_000274e8(g_extflash_spi_handle, &xfer) == 0) ? -1 : 0;
}

/* SPI receive. Same shape as TX but with mode=0 and a receive buffer.
 * OEM @ 0x00024418 (42 B). */
int extflash_spi_rx(void *buf, uint32_t len)
{
    extern void *g_extflash_spi_handle;  /* DAT_00024444 - 0xC */

    struct { uint32_t len; void *buf; uint32_t mode; } xfer;
    xfer.buf  = buf;
    xfer.len  = len;
    xfer.mode = 0;

    return (FUN_000274e8(g_extflash_spi_handle, &xfer) == 0) ? -1 : 0;
}

/* Check SRP0 protect bit. OEM @ 0x00023640 (34 B). */
int extflash_sw_wp_enabled(void)
{
    uint8_t status = 0;
    if (ti_semaphore_pend(g_extflash_state.bus_mutex, 0xFFFFFFFFu) == 0) {
        return -1;
    }
    if (extflash_wait_wip_clear(&status) != 0) {
        ti_semaphore_post(g_extflash_state.bus_mutex);
        return -1;
    }
    ti_semaphore_post(g_extflash_state.bus_mutex);
    return (status & 0x80u) ? 1 : 0;
}

/* Check block-protect bits. OEM @ 0x00023678 (34 B). */
int extflash_block_wp_enabled(void)
{
    uint8_t status = 0;
    if (ti_semaphore_pend(g_extflash_state.bus_mutex, 0xFFFFFFFFu) == 0) {
        return -1;
    }
    if (extflash_wait_wip_clear(&status) != 0) {
        ti_semaphore_post(g_extflash_state.bus_mutex);
        return -1;
    }
    ti_semaphore_post(g_extflash_state.bus_mutex);
    return (status & 0x3Cu) ? 1 : 0;
}

/* 4 KB-sector read-modify-write. Reads the sector containing `addr`,
 * merges the caller's `len` bytes at `src` into the in-sector offset,
 * erases the sector, and writes it back. Handles cross-sector spans
 * by looping. Returns 1 on success, 0 on failure.
 * OEM @ 0x000193C0 (76 B). */
int extflash_sector_write(uint32_t addr, uint32_t len, const void *src)
{
    extern void  *FUN_000259A4(void);  /* scratch buffer alloc */
    uint8_t      *scratch;
    int           ok;

    scratch = (uint8_t *)FUN_000259A4();
    if (scratch == NULL) {
        return 0;
    }

    uint32_t sector_mask = 0xFFFFF000u;  /* DAT_0001944C */
    uint32_t remaining   = len;
    const uint8_t *p     = (const uint8_t *)src;

    while (remaining != 0) {
        uint32_t sector_addr = addr & sector_mask;
        uint32_t chunk       = remaining;

        if (sector_addr != ((addr + remaining) & sector_mask)) {
            /* cross-sector — clamp to end of current sector */
            chunk = 0x1000u - (addr & 0xFFFu);
        }

        /* Read the full 4 KB sector, merge, erase, write back */
        if (extflash_read(sector_addr, 0x1000u, scratch) == 0) {
            ok = 0;
            break;
        }
        extflash_erase_range(sector_addr, 0x1000u);
        memcpy(scratch + (addr & 0xFFFu), p, chunk);
        if (extflash_write(sector_addr, 0x1000u, scratch) == 0) {
            ok = 0;
            break;
        }

        addr    += chunk;
        p       += chunk;
        remaining -= chunk;
        ok = 1;
    }

    ti_semaphore_post(*(uint32_t *)0x20000000);  /* DAT_00019448 semaphore post */
    return ok;
}

/* Open the external SPI flash driver. If already open, returns 1
 * immediately. Otherwise: initialises the PIN driver, copies SPI
 * params from the flash driver-header table, opens the SPI bus at
 * a slow bitrate, sends a Release-Power-Down (0xAB) command, waits
 * WIP clear, runs chip identification (REMS JEDEC probe). On success
 * closes the slow handle and reopens at production speed.
 * Returns 1 on success, 0 on failure.
 * OEM @ 0x000152FC (~170 B). */
int extflash_open(int unused)
{
    extern void  FUN_0001733C(void *pin_cfg, const void *pin_table);
    extern void  FUN_000269B4(void *spi_params_out);
    extern void *FUN_00022630(int idx, void *params);
    extern void  FUN_000274DE(void *handle);
    extern void  FUN_00025BC2(void *pin_cfg);
    extern int   extflash_identify_chip(void);
    extern void  thunk_EXT_FUN_1002CE00(int ms);       /* ROM sleep */

    char         *state = (char *)&g_extflash_state;
    void         *pin_cfg;
    void         *spi_handle;
    uint8_t       spi_params[32];
    uint8_t       rdp_cmd[4];
    int           rc;

    (void)unused;

    if (state[0] != 0) {
        return 1;  /* already open */
    }

    pin_cfg = state + 0x14;  /* PIN config field in extflash_state */
    FUN_0001733C(pin_cfg, (void *)(uintptr_t)0x0002A45C);  /* PIN table */
    FUN_000269B4(spi_params);

    spi_params[0] = 0;
    /* open SPI at slow bitrate (index 0) */
    spi_handle = FUN_00022630(0, spi_params);
    *(void **)(state + 8) = spi_handle;

    extflash_cs_deassert();
    thunk_EXT_FUN_1002CE00(10);  /* sleep 10 ms */

    rdp_cmd[0] = 0xAB;  /* Release Power-Down */
    extflash_cs_assert();
    rc = extflash_spi_tx(rdp_cmd, 1);
    extflash_cs_deassert();

    if (rc == 0) {
        /* busy-wait ~200 iterations */
        volatile int spin = 200;
        while (spin != 0) { spin--; }
        rc = extflash_wait_wip_clear(NULL);
    }

    if (rc == 0) {
        rc = extflash_identify_chip();
    }

    if (rc == 1) {
        /* reopen at production bitrate */
        FUN_000274DE(*(void **)(state + 8));
        *(uint32_t *)(spi_params + 4) = 0x00028000;  /* production bitrate */
        spi_handle = FUN_00022630(0, spi_params);
        state[0] = 1;
        *(void **)(state + 8) = spi_handle;
        return 1;
    }

    FUN_00025BC2(pin_cfg);
    FUN_000274DE(*(void **)(state + 8));
    return 0;
}
