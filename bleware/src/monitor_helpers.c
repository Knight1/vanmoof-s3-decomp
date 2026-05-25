/* monitor_helpers.c — debug-console utility functions.
 *
 * Small wrappers used by the monitor cmd_* handlers for timing,
 * string operations, and I/O.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include "bleware.h"

/* Sleep for `ms` milliseconds. Converts to 10 µs ticks (the TI-RTOS
 * Clock module's native unit) and calls the ROM sleep function.
 * OEM @ 0x00027542 (10 B). */
void monitor_sleep(uint32_t ms)
{
    extern void thunk_EXT_FUN_1002CE00(int ticks);
    thunk_EXT_FUN_1002CE00((int)ms * 100);
}

/* Yield one tick to the TI-RTOS scheduler. Called in tight loops
 * (log dump, audio operations) to keep other tasks alive.
 * OEM @ 0x00027478 (10 B). */
void monitor_yield_ticks(uint32_t ticks)
{
    (void)ticks;
    extern void FUN_0002751A(uint32_t arg);
    extern uint32_t g_monitor_yield_arg;
    FUN_0002751A(g_monitor_yield_arg);
}

uint32_t monitor_key_wait_with_timeout(void *timer_ctx, uint32_t period_us)
{
    (void)timer_ctx;
    (void)period_us;
    return 0;
}

/* Minimal sscanf — used only for simple integer parsing in monitor
 * commands. Returns 0 (no matches) in the stub build. */
int monitor_sscanf(const char *input, const char *fmt, ...)
{
    (void)input; (void)fmt;
    return 0;
}

/* Delegate to the TI runtime vsnprintf. The OEM calls FUN_00000BC0
 * which is the Thumb-mode TI _vsnprintf. */
int monitor_snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
    extern int FUN_00000BC0(va_list *ap, void *unused, void **buf_out,
                            const void *putc_fn, const void *putc_arg);
    va_list ap;
    va_start(ap, fmt);
    char *p = buf;
    int n = (int)size - 1;
    if (n < 0) n = 0;
    FUN_00000BC0(&ap, (void *)(uintptr_t)&p, (void **)&p, NULL, NULL);
    va_end(ap);
    if (size > 0) *p = '\0';
    return (int)(p - buf);
}

int monitor_strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

void format_size(uint32_t bytes, char *buf, unsigned int bufsz)
{
    (void)bytes; (void)buf; (void)bufsz;
}

uint32_t rtos_mem_get_stats(void *stats_out)
{
    (void)stats_out;
    return 0;
}

int snv_compact(uint32_t arg)
{
    (void)arg;
    return 0;
}

int snv_free_space_query(void)
{
    return 0;
}
