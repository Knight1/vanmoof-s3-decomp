/* monitor/dispatcher.c — runtime command-dispatch loop.
 *
 * The monitor's user-facing entry point: takes a user-input string,
 * walks the static command table at OEM `0x0002A0BC`, and calls each
 * registered handler with verb=2 (EXECUTE) until one returns 0
 * ("matched & consumed"). See `cmd_extflash.c`'s ABI header for the
 * full universal handler signature.
 *
 * OEM @ flash `0x00024B38` (36 B body + 4 B literal pool). The
 * monitor's higher-level read-line/tokenise plumbing (presumably in
 * `source/monitor/monitor.c`) is decoded separately; this file
 * contains only the dispatch loop.
 *
 * Return value:
 *   1  — some handler returned 0 (matched & executed)
 *   0  — table exhausted, no handler matched (or table empty)
 */

#include "bleware.h"
#include "monitor.h"

#include <stdint.h>
#include <stddef.h>

/* The registry: a NULL-terminated array of command handlers. The OEM
 * layout is a packed function-pointer table at flash `0x0002A0BC`
 * with 25 entries.
 *
 * The handlers themselves are scattered across the cmd_*.c source
 * files; the table lives in whichever TU defines this symbol (likely
 * `source/monitor/monitor.c` in the OEM tree). We declare it `extern`
 * so each cmd_*.c file can list its handler when it lands. Until
 * then a weak fallback at the bottom of this file provides an empty
 * table so the build links. */
extern const monitor_cmd_handler_t g_monitor_commands[];

int monitor_dispatch_loop(const char *user_input)
{
    const monitor_cmd_handler_t *cursor = g_monitor_commands;
    monitor_cmd_handler_t        handler = *cursor;

    if (handler == NULL) {
        return 0;
    }

    for (;;) {
        int rc = handler(2, (void *)user_input, NULL, 0);
        if (rc == 0) {
            return 1;
        }
        cursor++;
        handler = *cursor;
        if (handler == NULL) {
            return 0;
        }
    }
}

/* Weak empty registry — overridden once a real command table lands
 * in another TU. With the empty table, dispatch always returns 0
 * (no match), which keeps the rest of the monitor wiring testable
 * without any cmd_* handlers being present in the build. */
__attribute__((weak))
const monitor_cmd_handler_t g_monitor_commands[] = {
    NULL,
};

/* Emit one help-table row via monitor_log: format "    %-33s - %s\r\n"
 * with `name` (the command name) and `description`. Used by every
 * cmd_* handler's verb-0 (PRINT_HELP) path. OEM @ 0x00021244 (24 B). */
void monitor_print_help_line(const char *name, const char *description)
{
    extern void monitor_log(const char *file, int line,
                            const char *func, int level,
                            const char *fmt, ...);
    monitor_log("source/monitor/cmd_help.c", 0xE, NULL, 8,
                "    %-33s - %s\r\n", name, description);
}
