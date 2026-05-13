/* util.c — generic helpers (libc-style functions implemented locally
 * because the build is -nostdlib). */

#include "util.h"
#include <stdint.h>

/* OEM @ 0x08005D6C — memcpy with word-fastpath when both src and dst
 * are word-aligned, byte fallback otherwise. The OEM bx lr's without
 * restoring r0, so the function returns void (not void*). */
void memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        /* word-aligned path */
        while (n >= 4u) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d += 4;
            s += 4;
            n -= 4u;
        }
    }
    /* byte tail (or all bytes if misaligned) */
    while (n != 0u) {
        *d++ = *s++;
        n--;
    }
}
