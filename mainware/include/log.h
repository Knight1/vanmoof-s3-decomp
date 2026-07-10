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

/* Verify the circular log-buffer header CRC (SRAM 0x20037000); reset + note on
 * mismatch (OEM 0x0802968C, earlier mislabelled "config_crc_check"). */
void log_buffer_crc_check(void);

/* Reset/reinit the SRAM circular log buffer (OEM 0x0802957C). */
void log_buffer_reset(void);

/* Log the last wake reason (a WAKE_* token from the code cached at 0x20000004),
 * or wipe the log on a cold boot. OEM 0x0803DA3C. */
void log_wake_reason(void);

/* Append a NUL-terminated string into the SRAM circular log buffer; returns the
 * byte count or 0xFFFFFFFF if too long (OEM 0x0802963C). */
uint32_t log_emit_string(const char *s);

/* Enable the "log to APP sink" path: set the flag at SRAM 0x200001D6 = 1, read by
 * the log-upload state machine (OEM app_log_sink_enable, 0x080298DC). */
void app_log_sink_enable(void);

/* Dump the whole circular log buffer to the console, reformatting each line's
 * leading epoch into ">DD/HH:MM:SS " (OEM log_buffer_dump, 0x080296B8). */
void log_buffer_dump(void);

#endif
