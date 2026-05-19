#ifndef SHIFTERBOOT_UTIL_H
#define SHIFTERBOOT_UTIL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Word-fast `memcpy` with byte tail — same shape as the OEM helper at
 * `0x08001740` (36 B). Returns nothing (the OEM convention — `bx lr`
 * with the original `r0`, no explicit return value), which is
 * non-POSIX but matches how shifterware names its analog at OEM
 * `0x08005D6C`. The fast path triggers when `(dst | src) & 3 == 0`;
 * otherwise the function falls through to a byte loop. Safe to name
 * `memcpy` because the build uses `-nostdlib` (no libc symbol). */
void memcpy(void *dst, const void *src, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_UTIL_H */
