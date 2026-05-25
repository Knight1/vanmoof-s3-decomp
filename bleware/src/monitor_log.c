/* monitor_log.c — location-aware formatted logger for the debug console.
 *
 * Used by 77 call sites across the VanMoof codebase. In the OEM firmware
 * (~350 B at 0x00006D90), log lines are ECC-encrypted, timestamped via
 * the timekeeper, and dispatched through the BLE stack's ICall logger
 * service. This implementation handles the formatting side; dispatch
 * to the BLE stack is deferred until the TI SDK is vendored.
 *
 * Supported format specifiers: %s %d %u %x %X %02x %c %% %lu %ld %lx
 * Convention: emits a "[file:line] " prefix when both are provided.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

#define LOG_BUF_SZ  256u

/* Write a signed decimal integer into buf at *pos. */
static void fmt_int(char *buf, int *pos, int val, int bufsz)
{
    if (val < 0) {
        if (*pos < bufsz - 1) buf[(*pos)++] = '-';
        val = -val;
    }
    if (val == 0) {
        if (*pos < bufsz - 1) buf[(*pos)++] = '0';
        return;
    }
    char tmp[12];
    int  t = 0;
    while (val > 0) {
        tmp[t++] = '0' + (val % 10);
        val /= 10;
    }
    while (t-- > 0 && *pos < bufsz - 1) {
        buf[(*pos)++] = tmp[t];
    }
}

/* Write an unsigned hex integer into buf at *pos (no leading zeros). */
static void fmt_hex(char *buf, int *pos, unsigned int val, int bufsz)
{
    if (val == 0) {
        if (*pos < bufsz - 1) buf[(*pos)++] = '0';
        return;
    }
    char tmp[9];
    int  t = 0;
    while (val > 0) {
        int nyb = val & 0xF;
        tmp[t++] = nyb < 10 ? '0' + nyb : 'a' + nyb - 10;
        val >>= 4;
    }
    while (t-- > 0 && *pos < bufsz - 1) {
        buf[(*pos)++] = tmp[t];
    }
}

/* Emit a single char into buf at *pos if there is room. */
static void fmt_ch(char *buf, int *pos, char c, int bufsz)
{
    if (*pos < bufsz - 1) buf[(*pos)++] = c;
}

/* Location-aware variadic logger. The OEM calls this from every
 * monitor cmd_* handler and from most VanMoof error/status paths.
 * OEM @ 0x00006D90 (~350 B).
 *
 * This implementation formats into a 256-byte stack buffer and
 * discards the result (the real OEM encrypts and dispatches via
 * the BLE stack). When the TI SDK is vendored, the formatted buffer
 * will be posted as an ICall message to the logger service. */
void monitor_log(const char *file, int line, const char *fn, int level,
                 const char *fmt, ...)
{
    char buf[LOG_BUF_SZ];
    int  pos = 0;

    /* Emit "[file:line] " prefix */
    if (file != NULL && line > 0) {
        const char *s = file;
        while (*s && pos < (int)sizeof(buf) - 1) buf[pos++] = *s++;
        fmt_ch(buf, &pos, ':', (int)sizeof(buf));
        fmt_int(buf, &pos, line, (int)sizeof(buf));
        fmt_ch(buf, &pos, ' ', (int)sizeof(buf));
    }

    va_list ap;
    va_start(ap, fmt);

    if (fmt != NULL) {
        const char *p = fmt;
        while (*p && pos < (int)sizeof(buf) - 1) {

            if (*p != '%') {
                buf[pos++] = *p++;
                continue;
            }

            p++; /* skip '%' */

            int zero_pad = 0;
            if (*p == '0') { zero_pad = 1; p++; }

            int width = 0;
            if (*p >= '0' && *p <= '9') { width = *p++ - '0'; }

            switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) s = "(null)";
                while (*s && pos < (int)sizeof(buf) - 1) buf[pos++] = *s++;
                break;
            }
            case 'd':
                fmt_int(buf, &pos, va_arg(ap, int), (int)sizeof(buf));
                break;
            case 'u':
                fmt_int(buf, &pos, (int)va_arg(ap, unsigned int),
                        (int)sizeof(buf));
                break;
            case 'x':
            case 'X': {
                unsigned int val = va_arg(ap, unsigned int);
                if (zero_pad && width > 0) {
                    unsigned int mask = 0xFu << ((width - 1) * 4);
                    while (width-- > 0 && pos < (int)sizeof(buf) - 1) {
                        int nyb = (val & mask) >> (width * 4);
                        buf[pos++] = nyb < 10 ? '0' + nyb : 'a' + nyb - 10;
                        mask >>= 4;
                    }
                } else {
                    fmt_hex(buf, &pos, val, (int)sizeof(buf));
                }
                break;
            }
            case 'c':
                fmt_ch(buf, &pos, (char)va_arg(ap, int), (int)sizeof(buf));
                break;
            case '%':
                fmt_ch(buf, &pos, '%', (int)sizeof(buf));
                break;
            case 'l':
                p++;
                if (*p == 'u')
                    fmt_int(buf, &pos, (int)va_arg(ap, unsigned long),
                            (int)sizeof(buf));
                else if (*p == 'd')
                    fmt_int(buf, &pos, (int)va_arg(ap, long),
                            (int)sizeof(buf));
                else if (*p == 'x')
                    fmt_hex(buf, &pos, (unsigned int)va_arg(ap, unsigned long),
                            (int)sizeof(buf));
                break;
            default:
                fmt_ch(buf, &pos, '%', (int)sizeof(buf));
                fmt_ch(buf, &pos, *p,  (int)sizeof(buf));
                break;
            }
            p++;
        }
    }

    va_end(ap);
    buf[pos] = '\0';

    (void)fn; (void)level;
    /* TODO: dispatch `buf` to the BLE logger ICall service when the
     * TI SDK is vendored. The OEM calls icall_send_service_msg with
     * an ECC-encrypted payload here. */
}
