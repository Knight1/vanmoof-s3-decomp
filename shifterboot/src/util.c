/* util.c — small generic helpers used across shifterboot.
 *
 *   `memcpy` @ 0x08001740 (36 B) — word-fast + byte tail. The OEM
 *   aligns the fast path on `(dst | src) & 3 == 0`; this is the
 *   GCC-runtime `memcpy` idiom that several OEM modules call into
 *   (image-header copy via `image_verify_crc`, raw byte moves in
 *   the Modbus dispatch path, etc.).
 */

#include "util.h"

#include <stdint.h>

void memcpy(void *dst, const void *src, uint32_t count)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* Fast path when both pointers are word-aligned. */
    if ((((uintptr_t)d | (uintptr_t)s) & 0x3u) == 0u) {
        while (count >= 4u) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d += 4;
            s += 4;
            count -= 4u;
        }
    }
    /* Byte tail — or whole copy when alignment didn't match. */
    while (count > 0u) {
        *d++ = *s++;
        count--;
    }
}
