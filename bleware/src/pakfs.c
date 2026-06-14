/* pakfs.c — read-only "PACK" archive reader on external SPI flash.
 *
 * VanMoof stages a firmware-asset bundle (the ".pak" container) in the
 * external SPI-NOR flash region at offset 0x80000..0x200000. The bundle
 * begins with a 12-byte header:
 *
 *     header[0..3]  magic "PACK"        (0x50 0x41 0x43 0x4B)
 *     header[4..7]  dir_offset          (byte offset of the first
 *                                        directory entry, relative to
 *                                        the bundle base)
 *     header[8..11] dir_end             (the read cursor stops once it
 *                                        reaches this value)
 *
 * Directory entries are a fixed 0x40 bytes each; entry+0x3C holds the
 * member's payload size and entry+0x00 holds the NUL-terminated member
 * name (printed by the `pack_list` monitor command).
 *
 * State lives in a small singleton block (the OEM keeps it at RAM
 * 0x20005300). Only two handle slots exist, so at most two archives can
 * be open at once. Layout of the singleton:
 *
 *     +0x00 u8    initialized flag (1 once the slot table is cleared)
 *     +0x04 ..    handle slot 0 (0x10 bytes)
 *     +0x14 ..    handle slot 1 (0x10 bytes)
 *     +0x24 ..    0x40-byte scratch buffer for the most-recent entry
 *     +0x5C u32   running sum of opened-archive base addresses
 *                 (a debug/telemetry counter — see pakfs_read_next_entry)
 *
 * Per-handle layout (0x10 bytes):
 *
 *     +0x00 u32   base       (bundle base; the open argument)
 *     +0x04 u32   read_base  (base + header.dir_offset)
 *     +0x08 u32   dir_end    (header.dir_end; cursor stop value)
 *     +0x0C u32   cursor     (byte offset into the directory, advances
 *                             by 0x40 per entry)
 *
 * OEM source path string is "source/oad/pakfs.c"; the per-function log
 * tag strings ("pakfs_open", "pakfs_getmembers") live in the same
 * rodata table at flash 0x0002B11C.
 *
 * OEM functions:
 *   pakfs_open            @ 0x00015888  (126 B)
 *   pakfs_read_next_entry @ 0x000185B8  ( 90 B)
 *   pakfs_close           @ 0x00026A90  ( 20 B)
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* External-SPI-flash primitives (src/extflash.c). */
extern int  extflash_open(int unused);                                  /* FUN_000152FC */
extern void extflash_close(void);                                       /* FUN_0002758E */
extern int  extflash_read(uint32_t addr, uint32_t len, void *dst);      /* FUN_0001C5A4 */

/* memcmp / memset — TI CGT runtime clones (src/runtime.c). */
extern int   memcmp(const void *a, const void *b, unsigned int n);      /* FUN_00025490 */
extern void *memset(void *dst, int c, unsigned int n);                  /* FUN_0001B6B2 */

/* Location-aware logger (src/monitor_log.c). */
extern void  monitor_log(const char *file, int line, const char *fn,
                         int level, const char *fmt, ...);              /* FUN_00006D90 */

#define PAKFS_SRC_FILE     "source/oad/pakfs.c"
#define PAKFS_HEADER_BYTES 0x0Cu        /* "PACK" + dir_offset + dir_end */
#define PAKFS_ENTRY_BYTES  0x40u        /* fixed-size directory entry    */
#define PAKFS_LOG_FLASH    2            /* monitor_log level used here    */

/* PACK magic, little-endian "PACK" — OEM DAT_00015940. */
static const uint8_t k_pakfs_magic[4] = { 'P', 'A', 'C', 'K' };

/* A single open-archive handle. */
struct pakfs_handle {
    uint32_t base;        /* +0x00 bundle base offset                   */
    uint32_t read_base;   /* +0x04 base + header.dir_offset             */
    uint32_t dir_end;     /* +0x08 cursor stop value (header.dir_end)   */
    uint32_t cursor;      /* +0x0C running byte offset into directory   */
};

/* Singleton state (OEM keeps this at RAM 0x20005300). Two handle slots
 * plus a shared scratch buffer and the running base-sum counter. */
struct pakfs_state {
    uint8_t              initialized;      /* +0x00                      */
    uint8_t              _pad1[3];         /* +0x01                      */
    struct pakfs_handle  slot[2];          /* +0x04, +0x14               */
    uint8_t              scratch[PAKFS_ENTRY_BYTES]; /* +0x24            */
    uint32_t             total_base_sum;   /* +0x5C                      */
};

static struct pakfs_state s_pakfs;

/* Open/scan the PACK archive whose bundle base is `base` (e.g. 0x80000).
 * Allocates the first free handle slot (of two), reads and validates the
 * 12-byte header, and primes the directory read cursor.
 *
 * Returns the handle pointer on success, NULL if `base` is 0, no slot is
 * free, the flash access failed, or the magic was wrong.
 *
 * OEM @ 0x00015888. The OEM reads the 12-byte header into the spilled
 * argument-2/3/4 stack slots and reuses words 1 and 2 of that buffer as
 * `dir_offset`/`dir_end`; the canonical-but-equivalent form below reads
 * the header into an explicit local. The OEM also returns the header's
 * first word packed into the high half of an undefined8 — a stack-layout
 * artifact of its compiler, not a real second result, so the signature
 * here is a plain pointer. */
struct pakfs_handle *pakfs_open(uint32_t base)
{
    if (base == 0) {
        return NULL;
    }

    /* First call clears the slot table's two "base" fields and marks the
     * singleton initialized. OEM zeroes slot[0].base (+0x04) and
     * slot[1].base (+0x14) only — not the whole slots. */
    if (s_pakfs.initialized != 1) {
        s_pakfs.slot[0].base = 0;
        s_pakfs.slot[1].base = 0;
        s_pakfs.initialized = 1;
    }

    for (int i = 0; i < 2; i++) {
        struct pakfs_handle *h = &s_pakfs.slot[i];
        if (h->base != 0) {
            continue;   /* slot in use */
        }

        if (extflash_open(0) == 0) {
            monitor_log(PAKFS_SRC_FILE, 0x99, "pakfs_open", PAKFS_LOG_FLASH,
                        "Failed at accessing external flash");
            return NULL;
        }

        uint32_t header[3];   /* magic, dir_offset, dir_end */
        extflash_read(base, PAKFS_HEADER_BYTES, header);
        extflash_close();

        if (memcmp(header, k_pakfs_magic, sizeof k_pakfs_magic) != 0) {
            return NULL;   /* bad magic */
        }

        h->base      = base;
        h->read_base = header[1] + base;   /* base + dir_offset */
        h->dir_end   = header[2];
        h->cursor    = 0;
        return h;
    }

    return NULL;   /* no free slot */
}

/* Read the next 0x40-byte directory entry from an open handle into the
 * singleton scratch buffer, advancing the cursor. Returns the scratch
 * pointer on success, or NULL once the cursor reaches `dir_end` (the
 * cursor is also reset to 0 at end-of-archive, so a fresh walk can be
 * started by re-reading). On a flash-access failure logs to the
 * "pakfs_getmembers" tag and returns NULL.
 *
 * OEM @ 0x000185B8. The end-of-archive test compares the handle's
 * dir_end (+0x08) against the cursor (+0x0C). On a successful read the
 * OEM accumulates the handle base into the singleton's running sum at
 * +0x5C — preserved as a debug/telemetry counter. */
void *pakfs_read_next_entry(struct pakfs_handle *h)
{
    if (h == NULL) {
        return NULL;
    }

    if (h->dir_end == h->cursor) {
        h->cursor = 0;
        return NULL;
    }

    if (extflash_open(0) == 0) {
        /* OEM tag string lives at flash 0x0002B143 ("pakfs_getmembers"). */
        monitor_log(PAKFS_SRC_FILE, 0x62, "pakfs_getmembers", PAKFS_LOG_FLASH,
                    "Failed at accessing external flash");
        return NULL;
    }

    extflash_read(h->read_base + h->cursor, PAKFS_ENTRY_BYTES, s_pakfs.scratch);
    extflash_close();

    h->cursor += PAKFS_ENTRY_BYTES;
    s_pakfs.total_base_sum += h->base;

    return s_pakfs.scratch;
}

/* Release a PACK handle: zero its 0x10-byte struct (freeing the slot)
 * and return 1. Returns 0 if `h` is NULL.
 *
 * OEM @ 0x00026A90. */
int pakfs_close(struct pakfs_handle *h)
{
    if (h == NULL) {
        return 0;
    }
    memset(h, 0, sizeof *h);
    return 1;
}
