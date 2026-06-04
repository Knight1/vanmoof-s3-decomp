#include <stdint.h>

#include "log.h"
#include "panic.h"

/* The Muco runtime's fatal-assert path (OEM 0x0803DAC4). The OEM loads the
 * logger from its fixed SRAM slot (*0x20009D98 == g_log_func) and the format
 * string from rodata (0x08053300), calls `g_log_func(fmt, file, line)`, then
 * falls into a tight `b .` spin. We keep the spin verbatim; recovery is left
 * to the independent IWDG, exactly as in the OEM. */
_Noreturn void muco_assert_fail(const char *file, int line)
{
    g_log_func("FATAL error File [%s] line [%d]\r\n", file, line);
    for (;;) {
    }
}
