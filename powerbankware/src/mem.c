#include "powerbankware.h"

/*
 * Small memory helpers (OEM leaves, generic — shared across modules).
 *
 *   mem_zero    = OEM FUN_08011e7a   (byte fill with 0)
 *   mem_compare = OEM FUN_08011eb0   (equality test)
 */

/* Zero `len` bytes at `dst`. */
void mem_zero(void *dst, int len)
{
    uint8_t *p = (uint8_t *)dst;
    for (int n = len; n != 0; n--) {
        *p++ = 0;
    }
}

/*
 * Compare `len` bytes. The OEM counts matching bytes and returns
 * (matches == len), so it is a plain full-buffer equality test.
 */
bool mem_compare(const void *a, const void *b, uint16_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint16_t matches = 0;

    for (uint16_t i = 0; i < len; i++) {
        if (pa[i] == pb[i]) {
            matches++;
        }
    }
    return matches == len;
}
