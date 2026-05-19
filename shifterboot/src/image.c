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
#include "crc.h"
#include "util.h"

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

/* OEM @ 0x08000158 (100 B). Validate the OTA staging image at flash
 * `0x08001800`. Called twice from `main`: once at `0x0800027E`
 * inside the main-loop boot path (after both slots' magics have
 * been read) and once at `0x08000392` inside the cmd-0x81 ("apply
 * image") Modbus dispatch path.
 *
 * Algorithm (same as shifterware's `image_verify_crc` at OEM
 * `0x08003AC6` — both firmwares validate the same image format):
 *
 *   1. magic field (offset +0) must equal `0xAA55AA55`.
 *      → otherwise return 2 (header invalid)
 *   2. length field (offset +0xC) must be `< 0x3000` (12 KB).
 *      → otherwise return 2 (header invalid)
 *   3. Reset the hardware CRC engine, snapshot the 40 B header into
 *      a stack-local buffer with `memcpy`, blank the `crc` and
 *      `length` fields to `0xFFFFFFFF`, then feed the 10 u32 words
 *      of the masked header through `crc32_words`.
 *   4. Feed the remaining `(length - 40) / 4` payload words from
 *      flash `0x08001828` (= base + header_size) through
 *      `crc32_words`. The return value is the accumulated CRC.
 *   5. If the computed CRC equals the image's stored `crc` field
 *      (offset +8 — read from FLASH, not from the masked local
 *      copy), return 0. Otherwise return 1.
 *
 * Implementation note: the OEM emits two dead stores (writing the
 * original `crc` and `length` back into the local header buffer at
 * stack offsets +8 / +0xC, after the CRC computation completed).
 * Those bytes are never read back; gcc `-Os` will drop them. We
 * don't reproduce them here. */

/* The OEM points `g_image` at `0x08001800` via the pool word at
 * `0x080004D4` (also used by main + by `image_apply`). */
static const uint8_t *const g_image = (const uint8_t *)0x08001800u;

int image_verify_crc(void)
{
    /* Local 40-byte buffer for the CRC-masked header snapshot. */
    union {
        uint8_t  b[IMAGE_HDR_SIZE];
        uint32_t w[IMAGE_HDR_SIZE / 4u];
    } local;

    const uint32_t magic  = ((const uint32_t *)g_image)[0];
    const uint32_t length = ((const uint32_t *)g_image)[3];

    if (magic != IMAGE_MAGIC) {
        return IMAGE_HDR_INVALID;
    }
    if (length >= IMAGE_MAX_SIZE) {
        return IMAGE_HDR_INVALID;
    }

    CRC_ResetDR();

    memcpy(local.b, g_image, IMAGE_HDR_SIZE);
    local.w[2] = 0xFFFFFFFFu;   /* mask crc */
    local.w[3] = 0xFFFFFFFFu;   /* mask length */
    (void)crc32_words(local.w, IMAGE_HDR_SIZE / 4u);

    const uint32_t payload_words = (length - IMAGE_HDR_SIZE) / 4u;
    const uint32_t *payload = (const uint32_t *)(g_image + IMAGE_HDR_SIZE);
    const uint32_t got = crc32_words(payload, payload_words);

    const uint32_t stored_crc = ((const uint32_t *)g_image)[2];
    return (got == stored_crc) ? IMAGE_OK : IMAGE_CRC_MISMATCH;
}
