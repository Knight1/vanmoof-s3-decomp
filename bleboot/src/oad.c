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
    uint32_t image_hash;      /* offset  8 — expected value from hash compute */
    uint8_t  reserved_1[5];   /* offset 12 */
    uint8_t  status;          /* offset 17 — 0xFE = verified */
    uint8_t  reserved_2[6];   /* offset 18 */
    uint32_t image_addr;      /* offset 24 — input to size/hash helpers */
    uint32_t entry;           /* offset 28 — branch target on success */
    uint8_t  reserved_3[24];  /* offset 32..55 — vendor ID + signing block */
} oad_image_header_t;

_Static_assert(sizeof(oad_image_header_t) == 56,
               "OAD image header must be 56 bytes");

/* The five helpers still undecoded as of this commit. Names and
 * prototypes mirror Ghidra's `FUN_*` symbols so the next decomp pass
 * can rename in place without grep-replace collisions in this file.
 * The roles below are inferred from how they're used here:
 *
 *   FUN_00056a88 — "is there a candidate image at all?" precheck.
 *   FUN_000569e4 — flash read; (offset, n, dst).
 *   FUN_00056cb8 — derives an image-base/length word from the
 *                  header's `image_addr`. The return is shifted
 *                  right by 13 to get a flash page number, so the
 *                  result is a flash byte address.
 *   FUN_00056714 — secondary check on the header/body geometry
 *                  (likely a CRC32 over the image body).
 *   FUN_000560d8 — the 376-byte hash function (likely SHA-256-ish,
 *                  fed with `g_hw_id_cached` as a per-bike salt).
 *   FUN_00056e72 — flash program; (page, offset, src, n). Matches
 *                  TI driverlib `FlashProgram(buf, dst, n)` after
 *                  page/offset packing. */
extern int      FUN_00056a88(void);
extern void     FUN_000569e4(uint32_t off, uint32_t n, void *dst);
extern uint32_t FUN_00056cb8(uint32_t a, uint32_t img);
extern int      FUN_00056714(uint32_t a, uint32_t img, uint32_t size);
extern uint32_t FUN_000560d8(uint32_t page, uint32_t hw_id, uint32_t img, uint32_t z);
extern void     FUN_00056e72(uint32_t page, uint32_t off, void *src, uint32_t n);
extern void     FUN_00057156(uint32_t entry);
extern void     FUN_000570ac(void);

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

    uint32_t image_base = FUN_00056cb8(0, hdr.image_addr);
    if (FUN_00056714(0, hdr.image_addr, image_base) != 0) {
        FUN_000570ac();
        return;
    }

    uint32_t page = (image_base >> 13) & 0xFFu;
    uint32_t hash = FUN_000560d8(page, g_hw_id_cached, hdr.image_addr, 0);

    /* `status` is laid out so that &status can be passed to the flash
     * programmer as a 1-byte source. The default 0xFC is what the
     * compiler emits because the source has the initialiser inline
     * with the declaration — the value is dead on the mismatch path
     * but the store is unconditional in the assembly. */
    uint8_t status = 0xFCu;
    if (hdr.image_hash == hash) {
        status = 0xFEu;
        FUN_00056e72(page, 17, &status, 1);
        FUN_00057156(hdr.entry);
    }

    FUN_000570ac();
}
