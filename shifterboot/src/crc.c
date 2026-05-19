/* crc.c — MM32F031 hardware-CRC helpers used by image_verify_crc.
 *
 *   `crc32_words` @ 0x080013CC (32 B) — single-word feeder loop.
 *   The OEM materialises the CRC->DR address via a literal-pool
 *   word at `0x08001400` (= 0x40023000), feeds `count` u32 words
 *   from `src` into the engine, then reads the accumulated CRC
 *   back from the same register.
 *
 * The companion `CRC_ResetDR` (the MindMotion HAL leaf at
 * `0x080013AC`, 8 B, vendor-stock) is declared `extern` in
 * `crc.h`; it's the one-line `*(CRC + 0x08) = 1` that resets the
 * peripheral's data register to `0xFFFFFFFF` before a new
 * accumulate.
 */

#include "crc.h"

#include <stdint.h>

#define CRC_DR   (*(volatile uint32_t *)CRC_DR_ADDR)

uint32_t crc32_words(const uint32_t *src, uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        CRC_DR = src[i];
    }
    return CRC_DR;
}
