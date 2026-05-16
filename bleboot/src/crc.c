#include <stdint.h>

#include "bim.h"

/* CRC32-IEEE compute over an OAD image (`FUN_000560D8` in the OEM).
 *
 * Algorithm: standard reflected CRC32-IEEE — polynomial
 * `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR
 * `0xFFFFFFFF`. The first 12 bytes of the image are skipped (the
 * OAD identifier and length fields, which can't usefully cover
 * themselves). The per-byte update is delegated to `FUN_00056f50`,
 * a small routine that materialises one row of the standard CRC32
 * table by polynomial division — bit-for-bit equivalent to
 * `table[(crc ^ byte) & 0xFF]` for the standard CRC32-IEEE table.
 *
 * Buffer + chunking: image bytes are pulled into a 256-byte
 * SRAM scratch buffer at `0x20000300` and then CRC'd. The buffer
 * is filled by either:
 *
 *   - `bim_spi_flash_read` / `bim_iflash_read_paged` — flash read
 *     paths used when the caller passes `use_flash != 0`. Both
 *     BIM call sites pass `use_flash == 0`, so these paths are
 *     dead in the compiled image.
 *
 *   - `bim_memcpy_safe` — the alt source. Pure memcpy with a
 *     null-dst guard, no flash precheck or epilogue — fits the
 *     pattern of a RAM staging buffer where the OAD reception
 *     code accumulates incoming image bytes before they're
 *     committed to flash.
 *
 * Outer-loop chunking is driven by `chunk_size` (the caller passes
 * `g_oad_chunk_size`): the image is partitioned into chunks of
 * `chunk_size` bytes, each chunk spans `chunk_size / 256` blocks
 * of 256 bytes each. The middle loop iterates blocks, the inner
 * loop iterates bytes. The first block of the first chunk starts
 * at byte `skip_offset_base + 12` (the 12-byte OAD preamble skip);
 * every other block starts at byte 0.
 *
 * Security note: this is CRC32, **not** a cryptographic hash.
 * The check provides integrity (the BIM rejects an image whose
 * stored CRC doesn't match its computed CRC), but not authenticity
 * — anyone who can write to the OAD image bytes can also write the
 * matching CRC32 word. The `g_oad_chunk_size` value is a per-board
 * configuration, not a per-bike secret, so this offers no
 * per-device binding. */

#define BIM_OAD_BUFFER     ((uint8_t *)0x20000300u)
#define BIM_BUF_BYTES      256u
#define BIM_HEADER_SKIP    12u
#define BIM_MAX_FLASH_SIZE 0x100000u   /* 1 MB upper bound, use_flash path */
#define BIM_MAX_ALT_SIZE   0x58000u    /*   352 KB upper bound, alt path  */

/* CRC32-IEEE per-byte polynomial step (`FUN_00056F50` in the OEM).
 *
 * Performs 8 iterations of the canonical reflected CRC32 update:
 * shift the accumulator right by one, and XOR with the polynomial
 * `0xEDB88320` if the bit that fell off the LSB was 1. After 8
 * iterations the result is the CRC32 contribution of one byte of
 * input — equivalent to `crc32_table[mixed_byte]` for the standard
 * IEEE table, but materialised on demand instead of stored.
 *
 * Picking byte-step over table-lookup saves ~1 KB of flash (no
 * 256-entry table) at the cost of ~8 cycles per byte. The BIM is
 * size-constrained (8 KB flash page) and only CRCs at boot, so
 * the trade is sensible.
 *
 * Pure textbook algorithm — the polynomial constant is the only
 * thing in the function that identifies it; everything else is
 * the obvious 8-iteration loop. */
static __attribute__((noinline))
uint32_t crc32_ieee_byte_step(uint32_t mixed_byte)
{
    uint32_t crc = mixed_byte;
    int      n   = 8;

    if (n == 0) {
        return crc;
    }
    do {
        if (crc & 1u) {
            crc = (crc >> 1) ^ 0xEDB88320u;
        } else {
            crc = crc >> 1;
        }
        n = (uint8_t)(n - 1);
    } while (n != 0);

    return crc;
}

/* Matched-chip entry getter (`FUN_0005717C` in the OEM, 6 B +
 * 4 B literal). Returns the pointer at `g_chip_table_cursor`
 * (`0x20000408`) — which, after a successful
 * `bim_spi_probe_chip`, points at the matched 8-byte chip-table
 * entry inside `BIM_CHIP_TABLE_HEAD` (= `0x000571A8`). The first
 * dword of every entry is the chip's total capacity in bytes
 * (see flash.c chip-table layout commentary). `bim_crc32_buffer`
 * uses this to bound a CRC operation to the actual installed
 * chip's size — refusing to read past the chip's last addressable
 * byte. */
extern const uint8_t *bim_get_chip_entry(void);

uint32_t bim_crc32_image(uint32_t start_page,
                          uint32_t chunk_size,
                          uint32_t skip_offset_base,
                          uint32_t image_size,
                          uint8_t  use_flash)
{
    if (image_size == 0u || image_size == 0xFFFFFFFFu) {
        return 0u;
    }
    if (use_flash != 0u && image_size > BIM_MAX_FLASH_SIZE) {
        return 0u;
    }
    if (use_flash == 0u && image_size > BIM_MAX_ALT_SIZE) {
        return 0u;
    }

    uint8_t *buf = BIM_OAD_BUFFER;

    /* Initial fill of the scratch buffer — one of the two sources,
     * 256 bytes starting at the start_page-relative origin. */
    if (use_flash != 0u) {
        if (bim_flash_prepare() != 1) {
            return 0u;
        }
        bim_spi_flash_read(start_page << 12, BIM_BUF_BYTES, buf);
    } else {
        (void)bim_memcpy_safe(buf,
                              (const void *)(start_page * chunk_size),
                              BIM_BUF_BYTES);
    }

    uint32_t end_page = (start_page + (image_size - 1u) / chunk_size)
                        & 0xFFu;
    uint32_t last_chunk_bytes =
        ((image_size - 1u) % chunk_size) + 1u;

    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t page = start_page; page <= end_page; page++) {
        /* Number of 256-byte blocks to process for this chunk. */
        uint32_t blocks;
        if (page == end_page) {
            blocks = (last_chunk_bytes >> 8) & 0xFFu;
            if ((last_chunk_bytes & 0xFFu) != 0u) {
                blocks = (blocks + 1u) & 0xFFu;
            }
        } else {
            blocks = (chunk_size >> 8) & 0xFFu;
        }

        for (uint32_t blk = 0u; blk < blocks; blk++) {
            /* Refill `buf` for every block past the first (the
             * very first block was already loaded above). */
            if (blk != 0u) {
                if (use_flash != 0u) {
                    if (blk == blocks - 1u) {
                        /* The OEM does TWO loads here back-to-back:
                         * `bim_iflash_read_paged(page+1, 0, buf, 256)`
                         * (an IRQ-safe internal-flash memcpy) and then
                         * `bim_spi_flash_read((page<<12)+4096, 256, buf)`.
                         * The second overwrites the first, so only the
                         * SPI data survives. Preserved verbatim — the
                         * first call's only observable effect is the
                         * IRQ-disable bracket spanning the redundant
                         * memcpy. */
                        (void)bim_iflash_read_paged(page + 1u, 0u,
                                                    buf, BIM_BUF_BYTES);
                        bim_spi_flash_read((page << 12) + 4096u,
                                      BIM_BUF_BYTES, buf);
                    } else {
                        (void)bim_iflash_read_paged(page,
                                                    (blk + 1u) * 256u,
                                                    buf,
                                                    BIM_BUF_BYTES);
                    }
                } else {
                    (void)bim_memcpy_safe(
                        buf,
                        (const void *)(page * chunk_size
                                       + blk * 256u + 256u),
                        BIM_BUF_BYTES);
                }
            }

            /* Inner-loop byte count — full 256 for any non-final
             * block; for the final block of the final chunk, take
             * the low byte of `last_chunk_bytes` (or 256 if it's
             * exactly aligned). */
            uint32_t byte_count;
            if (page == end_page && blk == blocks - 1u) {
                uint8_t low = last_chunk_bytes & 0xFFu;
                byte_count = (low == 0u) ? 256u : (uint32_t)low;
            } else {
                byte_count = 256u;
            }

            /* Skip the 12-byte OAD preamble on the very first
             * iteration only. */
            uint32_t start_offset =
                (page == start_page && blk == 0u)
                    ? ((skip_offset_base + BIM_HEADER_SKIP) & 0xFFFFu)
                    : 0u;

            /* CRC32-IEEE byte step — `crc = TABLE[(crc^byte)&0xFF]
             * ^ (crc >> 8)`, with `FUN_00056f50` materialising the
             * table entry by per-bit polynomial division. */
            for (uint32_t i = start_offset; i < byte_count; i++) {
                uint32_t mixed = (crc ^ (uint32_t)buf[i]) & 0xFFu;
                crc = crc32_ieee_byte_step(mixed) ^ (crc >> 8);
            }
        }
    }

    if (use_flash != 0u) {
        bim_flash_release();
    }

    return crc ^ 0xFFFFFFFFu;
}

/* Flat-buffer CRC32-IEEE compute (`FUN_0005653C` in the OEM,
 * 158 B). Sibling of `bim_crc32_image` — same per-byte CRC32-IEEE
 * algorithm, same `crc32_ieee_byte_step` helper, but operating on
 * a single contiguous flash range `[addr, addr + len)` instead of
 * the paged OAD layout. Used by `bim_full_scan_and_launch` as the
 * primary integrity check against `hdr[8]` before the more
 * expensive paged CRC pass.
 *
 * Two sources, selected by `use_spi`:
 *
 *   - `use_spi != 0` — `bim_spi_flash_read(addr, 256, buf)`:
 *     reads from the external SPI NOR flash via the SSI0
 *     primitive. Bounded against the matched chip's capacity:
 *     `bim_get_chip_entry()` returns a pointer to the matched
 *     8-byte chip-table entry; its first dword is the chip size
 *     in bytes. If `len > chip_size` the function bails — refuses
 *     to CRC past the chip's last addressable byte. The release
 *     teardown (`bim_flash_release`) on the failure branch is
 *     defensive — the prepare side isn't paired here, suggesting
 *     the caller has already brought the SPI flash up via
 *     `bim_flash_prepare` and FUN_0005653C is only meant to drop
 *     the session on the early-bail failure path.
 *
 *   - `use_spi == 0` — `bim_memcpy_safe(buf, addr, 256)`: reads
 *     from the alt source via a defensive memcpy. Same source
 *     `bim_crc32_image` uses for its alt path; treated as
 *     offset-only with no SPI precheck. Dead in this build
 *     (every in-source caller passes `use_spi = 1`), but
 *     preserved for OEM fidelity.
 *
 * Inner loop: 256-byte chunks into a stack-allocated scratch
 * buffer (the OEM allocates it as a stack-frame local via
 * pc-relative literal `0x20000300` — the *same* address as
 * `bim_crc32_image`'s `BIM_OAD_BUFFER`. Since `bim_crc32_buffer`
 * runs only inside a single BIM thread of control, sharing the
 * scratch is safe; the BIM never reenters either CRC routine).
 *
 * Per-byte step is identical to `bim_crc32_image`:
 * `crc = crc32_ieee_byte_step((crc ^ byte) & 0xFF) ^ (crc >> 8)`.
 *
 * Returns the final CRC32-IEEE value (with the standard
 * `^ 0xFFFFFFFF` post-XOR) on success, `0` on any failure
 * (zero length, all-ones length, length exceeds chip capacity). */
uint32_t bim_crc32_buffer(uint32_t addr, uint32_t len, uint8_t use_spi)
{
    if (len == 0u || len == 0xFFFFFFFFu) {
        if (use_spi != 0u) {
            bim_flash_release();
        }
        return 0u;
    }

    if (use_spi != 0u) {
        const uint8_t *entry      = bim_get_chip_entry();
        uint32_t       chip_bytes = *(const uint32_t *)entry;
        if (chip_bytes < len) {
            bim_flash_release();
            return 0u;
        }
    }

    uint8_t *buf = BIM_OAD_BUFFER;
    uint32_t crc = 0xFFFFFFFFu;

    while (len != 0u) {
        uint32_t chunk = (len > BIM_BUF_BYTES) ? BIM_BUF_BYTES : len;

        if (use_spi != 0u) {
            bim_spi_flash_read(addr, BIM_BUF_BYTES, buf);
        } else {
            (void)bim_memcpy_safe(buf, (const void *)addr, BIM_BUF_BYTES);
        }

        addr += chunk;
        len  -= chunk;

        for (uint32_t i = 0u; i < chunk; i++) {
            uint32_t mixed = (crc ^ (uint32_t)buf[i]) & 0xFFu;
            crc = crc32_ieee_byte_step(mixed) ^ (crc >> 8);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}
