#include <stdint.h>

#include "app.h"
#include "crc.h"
#include "log.h"

/* The console-printf function pointer (SRAM 0x20009D98). Set once during
 * application init (the initialiser is not yet decoded) and then used by
 * every system-exception handler, the Muco assert, and the debug console.
 * Defined here as the canonical home; it is NULL until init assigns the real
 * logger. */
log_func_t g_log_func;

/* Fill a stack buffer with the current RTC time fields (OEM 0x080380A4):
 * [0]=hour, [1]=minute, [2]=second, [0x16]=day. */
extern void rtc_fill_time_fields(uint8_t *buf);

/* Print a "DD/HH:MM:SS " timestamp prefix (OEM log_print_timestamp_prefix,
 * 0x0803DBC8), emitted immediately before many log lines. The state flag byte
 * at 0x20000083 is saved, zeroed, and restored around the line (a plain byte
 * save/clear/restore via state_flag_get/set, NOT an interrupt mask). */
void log_print_timestamp_prefix(void)
{
    uint8_t buf[0x2C];
    uint8_t saved = state_flag_get();

    state_flag_set(0);
    rtc_fill_time_fields(buf);
    g_log_func("%02d/%02d:%02d:%02d ", buf[0x16], buf[0], buf[1], buf[2]);
    state_flag_set(saved);
}

/* Integrity-check the circular log-buffer control header (OEM, 0x0802968C —
 * earlier mislabelled "config_crc_check"). The header lives at SRAM 0x20037000,
 * 8 bytes guarded by a CRC-16 (seed 0xFFFF) stored at +8. On mismatch the buffer
 * is reset and a notice is written into it. */
void log_buffer_crc_check(void)
{
    volatile uint8_t *hdr = (volatile uint8_t *)0x20037000u;
    uint16_t calc = crc16((const uint8_t *)hdr, 8, 0xFFFFu);

    if (*(volatile uint16_t *)(hdr + 8) != calc) {
        log_buffer_reset();
        log_emit_string((const char *)0x0804FE3Cu);   /* "Log cleared because invalid CRC\r\n" */
    }
}

/* SRAM circular log-buffer control header @ 0x20037000; the payload starts at
 * the 12-byte header end (0x2003700C). The +0/+4 cursor roles are provisional —
 * the reset path sets both equal and the append path only compares them. */
typedef struct {
    uint8_t *read_cursor;    /* +0x00 */
    uint8_t *write_cursor;   /* +0x04 */
    uint16_t hdr_crc;        /* +0x08  CRC-16 over the 8 header bytes */
    uint8_t  overflow_flag;  /* +0x0A */
} log_ctrl_t;

#define LOG_CTRL    ((volatile log_ctrl_t *)0x20037000u)
#define LOG_PAYLOAD ((uint8_t *)0x2003700Cu)

extern void         *memset(void *dst, int c, unsigned int n);  /* newlib (vendor) */
extern unsigned int  strlen(const char *s);                     /* newlib (vendor) */

/* --- SRAM circular log-buffer byte primitives --- payload [0x2003700C,
 * 0x2004FC00) (~100 KB); read cursor @ header+0, write cursor @ header+4. */

/* Recompute + store the header CRC-16 (OEM log_buffer_header_crc_update,
 * 0x08029564); shared by the reset/append/pop paths to keep the header valid. */
void log_buffer_header_crc_update(void)
{
    *(volatile uint16_t *)(0x20037000u + 8) =
        crc16((const uint8_t *)0x20037000u, 8, 0xFFFFu);
}

/* Write one byte at the write cursor, advancing + wrapping at the end marker
 * and setting the overflow flag on wrap (OEM log_putc, 0x080295D8). */
void log_putc(uint8_t b)
{
    uint8_t **wr  = (uint8_t **)(0x20037000u + 4);
    uint8_t  *cur = *wr;

    *wr  = cur + 1;
    *cur = b;
    if (*wr > (uint8_t *)(0x20037000u + 0x18C00u)) {     /* past end → wrap */
        *wr = (uint8_t *)0x2003700Cu;
        *(volatile uint8_t *)(0x20037000u + 0x0A) = 1;   /* overflow flag */
    }
}

/* Read one byte at the read cursor, advancing + wrapping (OEM log_getc,
 * 0x080295B4). */
uint8_t log_getc(void)
{
    uint8_t **rd  = (uint8_t **)0x20037000u;
    uint8_t  *cur = *rd;
    uint8_t   b;

    *rd = cur + 1;
    b   = *cur;
    if (*rd > (uint8_t *)0x2004FC00u) {                  /* past end → wrap */
        *rd = (uint8_t *)0x2003700Cu;
    }
    return b;
}

/* Pop one line (up to and including '\n', or until the buffer empties) from the
 * read side; copies into out if non-NULL, returns the byte count, then re-stamps
 * the header CRC (OEM log_drain_line, 0x08029604). */
int log_drain_line(uint8_t *out)
{
    volatile uint32_t *hdr = (volatile uint32_t *)0x20037000u;
    int     count = 0;
    uint8_t c;

    do {
        if (hdr[0] == hdr[1]) {        /* read cursor == write cursor: empty */
            break;
        }
        c = log_getc();
        if (out != 0) {
            *out++ = c;
        }
        count++;
    } while (c != 0x0A);

    log_buffer_header_crc_update();
    return count;
}

/* Reset/reinit the SRAM circular log buffer (OEM log_buffer_reset, 0x0802957C):
 * point both cursors at the payload start, revalidate the header CRC, zero the
 * payload, and print "Log cleared" to the console. */
void log_buffer_reset(void)
{
    LOG_CTRL->write_cursor = LOG_PAYLOAD;
    LOG_CTRL->read_cursor  = LOG_PAYLOAD;
    log_buffer_header_crc_update();
    memset(LOG_PAYLOAD, 0, 0x18BF4u);          /* payload size 101876 bytes */
    g_log_func("Log cleared\r\n");
}

/* Append a NUL-terminated string into the SRAM circular log buffer (OEM
 * log_emit_string, 0x0802963C). Rejects strings >= 0x100 bytes; on overflow it
 * drops the oldest line to make room; revalidates the header CRC at the end.
 * Returns the byte count, or 0xFFFFFFFF if the string was too long. */
uint32_t log_emit_string(const char *s)
{
    uint32_t len = strlen(s);

    if (len >= 0x100u) {
        return 0xFFFFFFFFu;
    }

    for (uint32_t i = 0; i < len; i++) {
        log_putc((uint8_t)s[i]);
        if (LOG_CTRL->overflow_flag != 0) {
            if (LOG_CTRL->write_cursor == LOG_CTRL->read_cursor) {
                (void)log_getc();
                log_drain_line(0);
            }
        }
    }

    log_buffer_header_crc_update();
    return len;
}
