#include <stdint.h>

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

/* Get/set a flag byte at SRAM 0x20000083 (OEM 0x08036B8C / 0x08036B80). These
 * bracket the log line below; they are a plain byte save/clear/restore, NOT an
 * interrupt mask. */
extern uint8_t FUN_08036b8c(void);
extern void    FUN_08036b80(uint8_t v);

/* Print a "DD/HH:MM:SS " timestamp prefix (OEM log_print_timestamp_prefix,
 * 0x0803DBC8), emitted immediately before many log lines. */
void log_print_timestamp_prefix(void)
{
    uint8_t buf[0x2C];
    uint8_t saved = FUN_08036b8c();

    FUN_08036b80(0);
    rtc_fill_time_fields(buf);
    g_log_func("%02d/%02d:%02d:%02d ", buf[0x16], buf[0], buf[1], buf[2]);
    FUN_08036b80(saved);
}
