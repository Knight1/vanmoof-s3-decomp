/* util.c — miscellaneous standalone helpers.
 *
 * OEM source: source/misc/util.c (path string embedded at flash
 * 0x00020F68). Only two functions in the TU sit in this address band,
 * bracketed by the path-string rodata at 0x00020F68 and the next
 * (BLE event-queue) translation unit:
 *
 *   util_assert_fail  @ 0x00020F4C  (28 B body) — assert/panic handler
 *   util_atoi         @ 0x00020F98  (70 B body) — base-10 string-to-int
 *
 * util_atoi classifies characters through the firmware's C-runtime
 * `__ctype` lookup table (the TI CGT `_ctype` array at flash
 * 0x00029B2C). The table is indexed by `c + 1` — index 0 is the EOF
 * (-1) slot — and each byte is a bitmask of character classes. This
 * build's two classes that matter here are:
 *
 *   bit 3 (0x08)  whitespace  (space, \t, \n, \v, \f, \r)
 *   bit 2 (0x04)  decimal digit ('0'..'9')
 *
 * These match the standard TI runtime ctype layout, verified against
 * the live table bytes (space@+0x21 = 0x88, '0'..'9'@+0x31.. = 0x44).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware variadic logger — FUN_00006D90. Not in bleware.h; the
 * codebase convention is a per-TU extern (see gatt_write.c). */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* C-runtime character-class table (TI CGT `_ctype`, flash 0x00029B2C).
 * Indexed by `c + 1`; element 0 is the EOF slot. */
extern const uint8_t g_ctype_table[];   /* DAT_00020FE0 → 0x00029B2C */

#define CTYPE_DIGIT  0x04u   /* bit 2 — '0'..'9' */
#define CTYPE_SPACE  0x08u   /* bit 3 — whitespace */

/* Assert/panic handler. Logs the failing location through monitor_log
 * with the util.c path, source line 0x62, and the originating function
 * name "app_error_handler" (string at flash 0x0002B47D), then spins
 * forever — this never returns.
 *
 * The log format is "E:0x%08x|L:%04d|F:%s\r\n": the three caller
 * arguments are the error code, the caller's source line, and the
 * caller's function-name string, forwarded verbatim as the variadic
 * tail.
 *
 * OEM @ 0x00020F4C. */
void util_assert_fail(uint32_t error_code, int line, const char *fn_name)
{
    monitor_log("source/misc/util.c", 0x62, "app_error_handler", 2,
                "E:0x%08x|L:%04d|F:%s\r\n", error_code, line, fn_name);

    /* OEM tail is an unconditional `b .` — preserved as a hard spin. */
    for (;;) {
    }
}

/* Base-10 string-to-int (atoi). Skips leading whitespace, consumes an
 * optional '+'/'-' sign, then accumulates decimal digits as
 * n = n*10 + (c - '0'), negating the result when a '-' was seen.
 *
 * Character classification uses g_ctype_table indexed by `c + 1`,
 * exactly mirroring the OEM (`*(table + c + 1)`). No base prefix, no
 * overflow handling — a faithful libc-style atoi.
 *
 * OEM @ 0x00020F98. */
int util_atoi(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    int   value = 0;
    int   c;
    int   negative = 0;

    /* Skip leading whitespace. */
    while ((c = *p, (g_ctype_table[c + 1] & CTYPE_SPACE) != 0)) {
        p++;
    }

    /* Optional sign. OEM advances past either '+' or '-' and reloads
     * the next character before the digit loop. */
    if (c == '-') {
        negative = 1;
        c = *++p;
    } else if (c == '+') {
        c = *++p;
    }

    /* Accumulate decimal digits. */
    while ((g_ctype_table[c + 1] & CTYPE_DIGIT) != 0) {
        value = value * 10 + c;
        c = *++p;
        value -= '0';
    }

    return negative ? -value : value;
}
