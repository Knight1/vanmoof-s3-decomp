#include <stdint.h>

#include "bim.h"

/* The 56-byte OAD (Over-the-Air Download) image header that lives at
 * the start of every application slot the BIM can boot. Only the
 * fields this routine touches are named; everything else is opaque
 * filler that downstream helpers either ignore or consume via
 * offsets we haven't yet pinned down. Layout is byte-for-byte
 * consistent with TI's `imgHdr_t` from
 * `source/ti/common/cc26xx/oad/oad_image_header.h` in the
 * SimpleLink CC13x2/CC26x2 SDK, but trimmed to the fields whose
 * meaning is forced by the call sites here — naming the rest from
 * the SDK would be guesswork until those call sites get decoded. */
typedef struct __attribute__((packed)) {
    uint8_t  reserved_0[8];   /* offset  0 — OAD identifier + format */
    uint32_t image_crc;       /* offset  8 — expected CRC32-IEEE value */
    uint8_t  reserved_1[5];   /* offset 12 */
    uint8_t  status;          /* offset 17 — 0xFE = verified */
    uint8_t  reserved_2[6];   /* offset 18 */
    uint32_t image_size;      /* offset 24 — image length in bytes */
    uint32_t entry;           /* offset 28 — branch target on success */
    uint8_t  reserved_3[24];  /* offset 32..55 — vendor ID + signing block */
} oad_image_header_t;

_Static_assert(sizeof(oad_image_header_t) == 56,
               "OAD image header must be 56 bytes");

/* 44-byte short header read by the scan loops as a fast filter.
 * Shares offsets with `oad_image_header_t` so the named fields land
 * at the same logical position; the trailing 12 bytes of the full
 * 56-byte struct aren't consulted in the fast path. The `magic`
 * byte is at offset 14 in this view (vs offset 16 in the full
 * struct), because the scan loops align the short header to read
 * from a slightly different anchor — see the per-call commentary
 * in the scanners below. */
typedef struct __attribute__((packed)) {
    uint8_t  reserved_0[8];
    uint32_t image_crc;       /* offset  8 — expected CRC32 */
    uint8_t  magic_a;         /* offset 12 — must be 3 */
    uint8_t  magic_b;         /* offset 13 — must be 1 */
    uint8_t  reserved_1[2];
    uint8_t  magic_c;         /* offset 16 — must be 0xFE (full-scan view) */
    uint8_t  status;          /* offset 17 — 0xFE/0xFF accept, 0xFC reject */
    uint8_t  flags;           /* offset 18 — must be in {1,3,7} (quick-scan view) */
    uint8_t  reserved_2[5];
    uint32_t image_size;      /* offset 24 */
    uint32_t entry;           /* offset 28 */
    uint8_t  reserved_3[12];  /* offset 32..43 */
} oad_short_header_t;

_Static_assert(sizeof(oad_short_header_t) == 44,
               "OAD short header must be 44 bytes");

/* External helpers — names mirror Ghidra's `FUN_*` symbols so the
 * next decomp pass can rename in place without grep-replace
 * collisions. Roles below are inferred from call-site shape:
 *
 *   FUN_00056a88 — "is the BIM allowed to run a scan now?" precheck.
 *   FUN_000569e4 — flash read; (slot_anchor, n_bytes, dst).
 *   FUN_00056cb8 — derives an image-base address from the header's
 *                  `image_size`. Return shifted right by 13 = page.
 *   FUN_00056714 — secondary CRC check over the image body.
 *   FUN_00056e72 — flash program; (page, offset, src, n_bytes).
 *   FUN_0005653c — primary CRC compute; (dst_buf, src_addr, len).
 *   FUN_000567a0 — short flash write; (addr, n_bytes, src).
 *   FUN_00056e40 — small flash read; (addr, dst, n_bytes).
 *   FUN_00056f74 — 8-byte header sniff; returns 1 if it looks like
 *                  a real OAD header start.
 *   FUN_00056b1c — slot iterator state machine; (current_slot) →
 *                  next slot index, or -1 to stop.
 *   FUN_000570ac — common epilogue (probably watchdog kick).
 *   FUN_00057156 — image launcher / entry handoff; ABI is unusual
 *                  (loads SP from `*(entry + 4)` and `blx`-es the
 *                  same word — see panic.c notes). Treated as
 *                  returning normally in case the launch is staged
 *                  rather than terminal. */
extern int      FUN_00056a88(void);
extern void     FUN_000569e4(uint32_t off, uint32_t n, void *dst);
extern uint32_t FUN_00056cb8(uint32_t slot_base, uint32_t image_size);
extern int      FUN_00056714(uint32_t slot_base, uint32_t image_size, uint32_t image_base);
extern void     FUN_00056e72(uint32_t page, uint32_t off, void *src, uint32_t n);
extern uint32_t FUN_0005653c(uint32_t dst_buf, uint32_t src_addr, uint32_t len);
extern void     FUN_000567a0(uint32_t addr, uint32_t n_bytes, void *src);
extern void     FUN_00056e40(uint32_t addr, void *dst, uint32_t n_bytes);
extern int      FUN_00056f74(void *hdr_8b);
extern int      FUN_00056b1c(int slot);
extern void     FUN_000570ac(void);
extern void     FUN_00057156(uint32_t entry);

void bim_verify_and_launch_image(void)
{
    if (FUN_00056a88() == 0) {
        return;
    }

    oad_image_header_t hdr;
    FUN_000569e4(0, sizeof hdr, &hdr);

    if (hdr.status != 0xFEu) {
        return;
    }

    uint32_t image_base = FUN_00056cb8(0, hdr.image_size);
    if (FUN_00056714(0, hdr.image_size, image_base) != 0) {
        FUN_000570ac();
        return;
    }

    uint32_t page = (image_base >> 13) & 0xFFu;
    uint32_t crc  = bim_crc32_image(page,
                                     g_oad_chunk_size,
                                     0,
                                     hdr.image_size,
                                     0);

    /* `status` is laid out so that &status can be passed to the flash
     * programmer as a 1-byte source. The default 0xFC is what the
     * compiler emits because the source has the initialiser inline
     * with the declaration — the value is dead on the mismatch path
     * but the store is unconditional in the assembly. */
    uint8_t status = 0xFCu;
    if (hdr.image_crc == crc) {
        status = 0xFEu;
        FUN_00056e72(page, 17, &status, 1);
        FUN_00057156(hdr.entry);
    }

    FUN_000570ac();
}

/* Quick scan — walks slots 0..43 (stride = 1<<13 = 8 KB = one
 * CC2642 flash page), reads an 8-byte sniff via `FUN_00056f74` first,
 * then a 44-byte short header. If the status byte (offset 17) is
 * `0xFE` (already promoted to "verified" by an earlier full scan) or
 * `0xFF` (pristine, never touched), launches the entry word via
 * `FUN_00057156`. `0xFC` ("rejected") and anything else cause the
 * slot to be skipped. Called from bim_dispatch when the full scan
 * already returned 0 (no launch happened) — this is the fast path
 * that trusts the promotion marker without re-verifying.
 *
 * The `start_slot` argument lets the caller resume mid-iteration;
 * `bim_dispatch` always passes 0 (scan from the start). */
void bim_quick_scan_and_launch(int start_slot)
{
    union {
        uint8_t            sniff[8];
        oad_short_header_t hdr;
    } buf;

    int slot = start_slot;
    do {
        uint32_t slot_anchor = (uint32_t)slot << 13;

        FUN_00056e40(slot_anchor, &buf, 8);
        if (FUN_00056f74(&buf) != 1) {
            goto next;
        }

        FUN_00056e40(slot_anchor, &buf, 44);

        if (buf.hdr.flags != 1 && buf.hdr.flags != 3 && buf.hdr.flags != 7) {
            goto next;
        }
        if (buf.hdr.magic_a != 3 || buf.hdr.magic_b != 1) {
            goto next;
        }
        if (buf.hdr.status == 0xFCu) {
            goto next;
        }
        if (buf.hdr.status == 0xFFu) {
            FUN_00057156(buf.hdr.entry);
            goto next;
        }
        if (buf.hdr.status == 0xFEu) {
            FUN_00057156(buf.hdr.entry);
            /* fall through to next */
        }

    next:
        slot = (int)(int8_t)(slot + 1);
    } while (slot < 44);
}

/* Full scan — the first-boot or post-OAD-update path. Iterates slots
 * through the helper state machine `FUN_00056b1c` (which returns the
 * next slot to consider, or `-1` to stop), and for each slot:
 *
 *   1. Reads the 56-byte primary header at slot-anchor (stride 4 KB
 *      here, not 8 KB — the BIM keeps headers on a tighter grid than
 *      images).
 *   2. Filters on `hdr[16]==0xFE`, `hdr[12]==3`, `hdr[13]==1`, and
 *      `hdr[17] != 0xFC`.
 *   3. Runs the primary CRC `FUN_0005653c` and compares against
 *      `hdr[8]`.
 *   4. If matched, reads a 44-byte secondary header, derives the
 *      image base via `FUN_00056cb8`, runs the secondary CRC
 *      `FUN_00056714`, writes a transient `0xFC` marker to slot+16
 *      (in-progress), then runs the hash via `FUN_000560d8` (seeded
 *      with `g_hw_id_cached`) and compares against the secondary
 *      header's `hdr2[8]`.
 *   5. On hash match: writes `0xFE` to slot+17 (verified marker) and
 *      to `image_base>>13`'s flash page at offset 17 (so the image
 *      itself carries the same marker), then launches via
 *      `FUN_00057156(hdr2[28])`.
 *   6. On hash mismatch: writes `0xFC` to slot+17 (rejected).
 *
 * Returns -1 if the initial precheck `FUN_00056a88` fails, otherwise
 * 0 after walking every slot the iterator surfaces.
 *
 * Note on `slot_base`: in this build, the value the OEM derives as
 * the "slot base address" (variable held in r5) is initialised to 0
 * and never updated — every flash-write that the OEM expresses as
 * `slot_base + 17` / `slot_base + 16` therefore lands at absolute
 * flash address 17 / 16. Preserved as-is rather than removed; the
 * underlying helper `FUN_000567a0` may interpret a 0 anchor as
 * "current slot" via global state. Pin once `FUN_000567a0` is
 * decoded. */
int bim_full_scan_and_launch(void)
{
    if (FUN_00056a88() == 0) {
        return -1;
    }

    const uint32_t slot_base = 0u;
    int slot = 0;

    for (;;) {
        slot = FUN_00056b1c((int)(uint8_t)slot);
        if (slot < 0) {
            break;
        }

        uint32_t slot_anchor = (uint32_t)slot << 12;

        /* Primary 56-byte header read from the slot at the 4 KB
         * stride. The fields checked here live at offsets that don't
         * coincide cleanly with `oad_image_header_t` — keep raw
         * byte access for clarity. */
        uint8_t hdr1[56];
        FUN_000569e4(slot_anchor, sizeof hdr1, hdr1);

        if (hdr1[16] != 0xFEu) continue;
        if (hdr1[12] != 3u)    continue;
        if (hdr1[13] != 1u)    continue;
        if (hdr1[17] == 0xFCu) continue;

        uint32_t image_size_primary = *(const uint32_t *)&hdr1[24];
        uint32_t image_crc_primary  = *(const uint32_t *)&hdr1[8];

        uint32_t crc1 = FUN_0005653c(slot_base + 12,
                                      image_size_primary - 12,
                                      1u);
        if (image_crc_primary != crc1) {
            continue;
        }
        /* The OEM re-checks `hdr1[16] == 0xFE` here — defensive
         * recheck against a register being clobbered across the CRC
         * call. Already verified above, so the check is dead. */

        /* Secondary 44-byte header read from `slot_base` (= 0 in this
         * build, so always reading from flash address 0 — the BIM
         * uses a fixed metadata anchor regardless of which slot is
         * being verified). */
        uint8_t hdr2[44];
        FUN_000569e4(slot_base, sizeof hdr2, hdr2);

        uint32_t image_size_sec = *(const uint32_t *)&hdr2[24];
        uint32_t image_end_sec  = *(const uint32_t *)&hdr2[36];
        uint32_t image_crc_sec  = *(const uint32_t *)&hdr2[8];
        uint32_t entry_sec      = *(const uint32_t *)&hdr2[28];
        uint8_t  flags_sec      = hdr2[18];

        uint32_t image_base = FUN_00056cb8(slot_base, image_size_sec);
        if (image_base == (uint32_t)-1) {
            continue;
        }

        /* The OEM rewrites the in-buffer copy of `image_size_sec`
         * with a derived length before the next CRC call. We mirror
         * that side-effect with a local — the in-memory buffer
         * rewrite is a compiler artifact of the local variable; the
         * derived length is what FUN_00056714 actually consumes. */
        uint32_t derived_len = image_end_sec - image_base + 1u;
        *(uint32_t *)&hdr2[24] = derived_len;

        int crc2_status =
            FUN_00056714(slot_base, derived_len, image_base);

        uint8_t status = 0xFCu;
        FUN_000567a0(slot_anchor + 16u, 1u, &status);

        if ((uint8_t)crc2_status != 0u) {
            continue;
        }

        uint32_t page = (image_base >> 13) & 0xFFu;
        uint32_t crc  = bim_crc32_image(page,
                                         g_oad_chunk_size,
                                         slot_base,
                                         derived_len,
                                         0);

        status = 0xFEu;
        if (image_crc_sec == crc) {
            /* Promote: write 0xFE both to the slot's metadata
             * (slot << 13 stride here, not 4 KB — the BIM's two
             * grids differ) and to the image's own header at
             * `image_base + 17`. */
            FUN_000567a0((slot_anchor << 1) + 17u, 1u, &status);
            FUN_00056e72(page, 17u, &status, 1u);

            /* Launch if the `flags` field is in {0,1,3,7}. The
             * trailing `status == 0xFE` recheck is always true in
             * this build — kept for OEM fidelity. */
            uint8_t f = flags_sec;
            if ((f & ~1u) == 0u || f == 3u || f == 7u) {
                FUN_000570ac();
                FUN_00057156(entry_sec);
            }
        } else {
            status = 0xFCu;
            FUN_000567a0(slot_base + 17u, 1u, &status);
        }

        status = 0xFCu;
        FUN_000567a0(slot_base + 16u, 1u, &status);
    }

    FUN_000570ac();
    return 0;
}
