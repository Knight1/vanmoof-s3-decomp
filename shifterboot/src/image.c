/* image.c — halfword-stream helpers for image-header parsing.
 *
 * Three leaves, all VanMoof-custom, decomp'd from shifterboot.bin:
 *
 *   - `image_read_halfword` @ 0x080014F8 (6 B) — a single `*p`
 *     halfword load. Non-inlined in the OEM build because the
 *     MindMotion BSP is compiled at -O0; `image_copy_halfwords`
 *     calls it once per iteration, and `main` calls it once
 *     directly at 0x08000260.
 *
 *   - `image_copy_halfwords` @ 0x08001554 (34 B) — `memcpy` for
 *     halfword-aligned buffers, used both by `image_read_u32_le`
 *     below and by `FUN_080016A6` (still pending). The compiler
 *     emits `uxth` on the index each iteration, fixing `count` at
 *     16 bits.
 *
 *   - `image_read_u32_le` @ 0x080001BC (28 B) — assemble a 32-bit
 *     little-endian value from two halfword reads. The result is
 *     also left in the caller-provided `staging` buffer; `main`
 *     calls this six times, once per u32 field of the image header
 *     (`magic`, `version`, `crc`, `length`, and two further fields
 *     that decomp will identify later).
 *
 * The OEM avoids a single 32-bit `ldr` for the assembled value
 * because Cortex-M0 requires 4-byte alignment for `ldr`; reading
 * two halfwords and OR'ing tolerates a source that is only 16-bit
 * aligned (which the staging buffer is, but not necessarily more).
 */

#include "image.h"

#include <stdint.h>

/* OEM @ 0x080014F8. The OEM body has a redundant `mov r1, r0`
 * before the load (a -O0 artefact); we emit the obvious one-line
 * dereference and let -Os pick the encoding. */
uint16_t image_read_halfword(const uint16_t *p)
{
    return *p;
}

/* OEM @ 0x08001554. Decrement-then-test loop with the index
 * `uxth`-clamped, so `count` is `uint16_t`. The OEM increments
 * the source pointer by 2 *outside* the read helper — this
 * mirrors that pattern. */
void image_copy_halfwords(const uint16_t *src,
                          uint16_t *dst,
                          uint16_t count)
{
    for (uint16_t i = 0u; i < count; i = (uint16_t)(i + 1u)) {
        dst[i] = image_read_halfword(src);
        src++;
    }
}

/* OEM @ 0x080001BC. Two halfword reads + shift/OR rather than one
 * 32-bit load — see the file-level comment for why. The OEM
 * compiles this as:
 *     ldrh r0, [r5, #2]    ; staging[1]
 *     lsls r0, r0, #16
 *     ldrh r1, [r5, #0]    ; staging[0]
 *     orrs r0, r1
 * so the byte order is `(staging[1] << 16) | staging[0]` (i.e. the
 * source stream is little-endian halfword-then-halfword, which is
 * the natural LE u32 layout). */
uint32_t image_read_u32_le(const uint16_t *src, uint16_t *staging)
{
    image_copy_halfwords(src, staging, 2u);
    return ((uint32_t)staging[1] << 16) | (uint32_t)staging[0];
}
