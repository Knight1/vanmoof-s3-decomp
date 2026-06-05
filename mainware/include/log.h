#ifndef MAINWARE_LOG_H
#define MAINWARE_LOG_H

/* The console-printf function pointer. Lives at SRAM 0x20009D98, set
 * once during init and called by every system-exception handler plus
 * the debug-console handlers. Format-string + varargs; signature
 * matches `printf` (return value is not consulted at any call site
 * we've seen). */
typedef int (*log_func_t)(const char *fmt, ...);

extern log_func_t g_log_func;

/* Emit a "DD/HH:MM:SS " timestamp prefix via g_log_func (OEM 0x0803DBC8). */
void log_print_timestamp_prefix(void);

#endif
