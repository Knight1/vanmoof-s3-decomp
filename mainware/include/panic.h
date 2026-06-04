#ifndef MAINWARE_PANIC_H
#define MAINWARE_PANIC_H

/* Muco-runtime fatal-assert handler (entry 0x0803DAC4). Called from ~10
 * sites across the image whenever an internal invariant breaks (e.g. the
 * scheduler running out of slots). It logs a "FATAL error" line through
 * g_log_func and then spins forever — the watchdog reboots the board.
 *
 *   muco_assert_fail(file, line)
 *     -> g_log_func("FATAL error File [%s] line [%d]\r\n", file, line);
 *        for (;;) { }
 *
 * The format string lives in rodata at 0x08053300; the `file` argument is
 * the caller's source path (e.g. "src/time.c" for the scheduler). */
_Noreturn void muco_assert_fail(const char *file, int line);

#endif
