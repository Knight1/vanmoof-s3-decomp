/* ota.c — shifterboot's per-chunk OTA flasher.
 *
 * One function decomp'd from shifterboot's own disassembly:
 *
 *   `ota_program_chunk` @ 0x08001658 (78 B)
 *     — packs the next 16 little-endian halfwords from an inbound
 *       Modbus frame's payload into the SRAM scratch buffer at
 *       0x200000F2, then writes the buffer to flash at `dst`
 *       via `flash_program_range`.
 *
 * Called from `main`'s cmd-0x82 OTA streaming loop
 * (`bl 0x08001658` at PC `0x080004B6`); each call advances `main`'s
 * `dst` cursor by `0x20` (= 32 bytes), so the inbound image is
 * deposited 32 bytes at a time starting at `0x08001800` (slot 1).
 */

#include "ota.h"
#include "flash.h"   /* flash_program_range */

#include <stdint.h>

/* The Modbus frame header is 11 bytes; the OTA payload starts at
 * offset 11. Materialised by the OEM as `adds r4, #0xB` after
 * loading `r4` with the frame base. */
#define OTA_FRAME_HEADER_OFFSET   11u

/* Per-chunk flash write size: always 16 halfwords (= 32 bytes).
 * Materialised by the OEM as `movs r2, #0x10` immediately before
 * the `bl flash_program_range`. */
#define OTA_CHUNK_HALFWORDS       16u

/* Same SRAM scratch buffer that `flash_copy_region` uses
 * (pool word at `0x0800170C` = `0x200000F2`). Sharing this buffer
 * is intentional — the two functions are never called concurrently
 * (one applies a finalised image; the other streams a new one). */
#define OTA_SCRATCH               ((uint16_t *)0x200000F2u)

void ota_program_chunk(uint32_t dst,
                       const uint8_t *frame,
                       uint8_t count_bytes)
{
    /* The OEM uses uint8 counters (`uxtb` after each increment) so
     * stream positions wrap at 256; we mirror the type. */
    uint8_t buf_idx = 0u;
    uint8_t stream_pos = 0u;
    const uint8_t *p = frame + OTA_FRAME_HEADER_OFFSET;

    while (stream_pos < count_bytes) {
        const uint8_t lo = p[0];
        const uint8_t hi = p[1];
        OTA_SCRATCH[buf_idx] = (uint16_t)((uint16_t)lo
                                          | ((uint16_t)hi << 8));
        p += 2;
        buf_idx    = (uint8_t)(buf_idx + 1u);
        stream_pos = (uint8_t)(stream_pos + 2u);
    }

    flash_program_range(dst, OTA_SCRATCH, OTA_CHUNK_HALFWORDS);
}
