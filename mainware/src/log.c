#include "log.h"

/* The console-printf function pointer (SRAM 0x20009D98). Set once during
 * application init (the initialiser is not yet decoded) and then used by
 * every system-exception handler, the Muco assert, and the debug console.
 * Defined here as the canonical home; it is NULL until init assigns the real
 * logger. */
log_func_t g_log_func;
