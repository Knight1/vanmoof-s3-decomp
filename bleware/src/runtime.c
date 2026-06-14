/* runtime.c — standard C library functions used by bleware.
 *
 * The OEM (TI CGT/armcc) provides optimized implementations of memcpy,
 * memset, and memcmp. These are the decompiled equivalents. strtol
 * (monitor_strtol) is a TI runtime clone used by log_seek_to_timestamp.
 *
 * OEM addresses:
 *   memcpy          @ 0x00018654  (~180 B)
 *   memset          @ 0x0001B6B2  (~110 B)
 *   memcmp          @ 0x00025490  (~40 B)
 *   monitor_strtol  @ 0x000107BC  (~180 B)
 */

#include <stdint.h>
#include <stddef.h>

/* ---- memcpy ----------------------------------------------------------
 * OEM @ 0x00018654. Optimized word-aligned copy with unaligned handling.
 */

void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (n == 0) return dst;

    if (((uintptr_t)s & 3u) == 0) {
        /* src aligned — fast word copy if dst also aligned */
        if (((uintptr_t)d & 3u) != 0) goto bytewise_until_aligned;

        /* 16-byte chunks */
        while (n >= 16) {
            uint32_t w0 = *(const uint32_t *)(s +  0);
            uint32_t w1 = *(const uint32_t *)(s +  4);
            uint32_t w2 = *(const uint32_t *)(s +  8);
            uint32_t w3 = *(const uint32_t *)(s + 12);
            *(uint32_t *)(d +  0) = w0;
            *(uint32_t *)(d +  4) = w1;
            *(uint32_t *)(d +  8) = w2;
            *(uint32_t *)(d + 12) = w3;
            s += 16; d += 16; n -= 16;
        }
        /* remaining words: 12, 8, 4 */
        if (n >= 12) { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; }
        if (n >= 8)  { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; }
        if (n >= 4)  { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; }
        /* tail bytes */
        while (n) { *d++ = *s++; n--; }
        return dst;
    }

    /* src unaligned — byte copy until dst aligned */
    do {
        *d++ = *s++;
        n--;
        if (n == 0) return dst;
    } while (((uintptr_t)s & 3u) != 0);

bytewise_until_aligned:
    if (((uintptr_t)d & 1u) == 0) {
        /* dst halfword-aligned — can do 2-byte stores from word reads */
        if (((uintptr_t)d & 3u) != 0) {
            /* dst aligned to 2 but not 4 — store as halfwords */
            while (n >= 4) {
                uint32_t w = *(const uint32_t *)s;
                *(uint16_t *)d         = (uint16_t)w;
                *(uint16_t *)(d + 2)   = (uint16_t)(w >> 16);
                s += 4; d += 4; n -= 4u;
            }
        } else {
            /* dst 4-byte aligned, src unaligned — full word copy */
            while (n >= 16) {
                uint32_t w0 = *(const uint32_t *)(s +  0);
                uint32_t w1 = *(const uint32_t *)(s +  4);
                uint32_t w2 = *(const uint32_t *)(s +  8);
                uint32_t w3 = *(const uint32_t *)(s + 12);
                *(uint32_t *)(d +  0) = w0;
                *(uint32_t *)(d +  4) = w1;
                *(uint32_t *)(d +  8) = w2;
                *(uint32_t *)(d + 12) = w3;
                s += 16; d += 16; n -= 16;
            }
            if (n >= 12) { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; n -= 4; }
            if (n >= 8)  { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; n -= 4; }
            if (n >= 4)  { uint32_t w = *(const uint32_t *)s; *(uint32_t *)d = w; s += 4; d += 4; n -= 4; }
        }
    } else {
        /* dst byte-unaligned — store as bytes from word reads */
        while (n >= 4) {
            uint32_t w = *(const uint32_t *)s;
            d[0] = (uint8_t) w;
            d[1] = (uint8_t)(w >> 8);
            d[2] = (uint8_t)(w >> 16);
            d[3] = (uint8_t)(w >> 24);
            s += 4; d += 4; n -= 4;
        }
    }

    /* tail bytes */
    while (n) { *d++ = *s++; n--; }
    return dst;
}

/* ---- memset ----------------------------------------------------------
 * OEM @ 0x0001B6B2. Word-aligned fill with unaligned preamble.
 */

void *memset(void *dst, int c, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t  fill = (uint8_t)c;

    if (n == 0) return dst;

    /* byte-by-byte until word-aligned */
    while (n && ((uintptr_t)d & 3u)) {
        *d++ = fill;
        n--;
    }
    if (n == 0) return dst;

    /* replicate fill byte into a word */
    uint32_t w = fill | ((uint32_t)fill << 8);
    w |= w << 16;

    /* 16-byte chunks */
    while (n >= 16) {
        *(uint32_t *)(d +  0) = w;
        *(uint32_t *)(d +  4) = w;
        *(uint32_t *)(d +  8) = w;
        *(uint32_t *)(d + 12) = w;
        d += 16; n -= 16;
    }
    /* 8-byte chunk */
    if (n & 8) {
        *(uint32_t *)(d + 0) = w;
        *(uint32_t *)(d + 4) = w;
        d += 8;
    }
    /* 4-byte chunk */
    if (n & 4) {
        *(uint32_t *)d = w;
        d += 4;
    }
    /* 2-byte tail */
    if (n & 2) {
        *(uint16_t *)d = (uint16_t)w;
        d += 2;
    }
    /* 1-byte tail */
    if (n & 1) {
        *d = (uint8_t)w;
    }
    return dst;
}

/* ---- memcmp ----------------------------------------------------------
 * OEM @ 0x00025490. Byte-by-byte comparison.
 */

int memcmp(const void *a, const void *b, unsigned int n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (unsigned int i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

/* ---- strtol (monitor_strtol) ----------------------------------------
 * OEM @ 0x000107BC. TI runtime strtol clone. Supports bases 2..36,
 * optional '0x'/'0X' hex prefix, leading whitespace skip, +/- sign.
 * Sets errno=0x22 (EINVAL) on overflow, returns LONG_MAX/LONG_MIN.
 */

#include <limits.h>

/* ASCII character classification table — OEM DAT_000108E8 */
static const uint8_t s_ctype[256] = {
    [' ']  = 0x08, ['\t'] = 0x08, ['\n'] = 0x08, ['\r'] = 0x08,
    ['+']  = 0x00, ['-']  = 0x00,
    ['0']  = 0x44, ['1'] = 0x44, ['2'] = 0x44, ['3'] = 0x44,
    ['4']  = 0x44, ['5'] = 0x44, ['6'] = 0x44, ['7'] = 0x44,
    ['8']  = 0x44, ['9'] = 0x44,
    ['A']  = 0x41, ['B'] = 0x41, ['C'] = 0x41, ['D'] = 0x41,
    ['E']  = 0x41, ['F'] = 0x41,
    ['a']  = 0x42, ['b'] = 0x42, ['c'] = 0x42, ['d'] = 0x42,
    ['e']  = 0x42, ['f'] = 0x42,
    ['x']  = 0x00, ['X']  = 0x80,  /* x: just space, X: hex marker */
};
/* OEM ctype bits (table at flash 0x00029B2D): bit0 = hex A-F, bit1 = hex
 * a-f, bit2 = decimal digit, bit3 = whitespace, bit6 = any hex digit.
 * All hex chars (0x44/0x41/0x42) carry bit6, so it marks "is hex". */

static int s_isspace(uint8_t c)  { return (s_ctype[c] >> 3) & 1; }
static int s_isdigit(uint8_t c)  { return (s_ctype[c] >> 2) & 1; }
static int s_ishex_low(uint8_t c) { return (s_ctype[c] >> 1) & 1; }   /* bit1 = a-f */
static int s_ishex_up(uint8_t c)  { return  s_ctype[c]       & 1; }   /* bit0 = A-F */
static int s_ishex(uint8_t c)     { return (s_ctype[c] >> 6) & 1; }   /* bit6 = any hex */

unsigned int monitor_strtol(const uint8_t *nptr, uint8_t **endptr, int base)
{
    const uint8_t *p = nptr;
    int neg = 0;
    unsigned int val = 0;
    int digits = 0;

    /* skip whitespace */
    while (s_isspace(*p)) p++;

    /* sign */
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    /* base detection */
    if (base == 0 || base == 16) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X') && s_ishex(p[2])) {
            p += 2;
            base = 16;
        }
    }
    if (base == 0) {
        if (*p == '0') {
            base = 8;
            if (p[1] >= '0' && p[1] <= '7') p++;
        } else {
            base = 10;
        }
    }

    /* digit accumulation with overflow detection.
     * OEM @ 0x00010858: the accumulation limit is (neg + 0x7FFFFFFF), so a
     * negative input may reach magnitude 0x80000000 (LONG_MIN) while a
     * positive input is capped at 0x7FFFFFFF (LONG_MAX). Preserve the
     * sign-dependent threshold verbatim. */
    unsigned int limit  = 0x7FFFFFFFu + (unsigned int)neg;
    unsigned int cutoff = limit / (unsigned int)base;
    unsigned int cutlim = limit % (unsigned int)base;

    while (1) {
        unsigned int digit;
        uint8_t c = *p;

        if (s_isdigit(c)) {
            digit = c - '0';
        } else if (s_ishex_low(c)) {
            digit = c - 'a' + 10;
        } else if (s_ishex_up(c)) {
            digit = c - 'A' + 10;
        } else {
            break;
        }

        if (digit >= (unsigned int)base) break;

        if (val > cutoff || (val == cutoff && digit > cutlim)) {
            /* overflow */
            if (endptr) *endptr = (uint8_t *)nptr;
            return neg ? 0x80000000u : 0x7FFFFFFFu;
        }

        val = val * (unsigned int)base + digit;
        digits++;
        p++;
    }

    if (digits == 0 && endptr) {
        *endptr = (uint8_t *)nptr;
    } else if (endptr) {
        *endptr = (uint8_t *)p;
    }

    return neg ? -val : val;
}
