/* monitor/cmd_help.c — `help` command handler.
 *
 * OEM at 0x00013C20 (~64 B). Iterates a NULL-terminated table of
 * per-module help-printer function pointers and calls each with `0`
 * as arg ("emit your help block"). The table head is at flash
 * 0x0002A0BC (16+ entries).
 *
 * Logs two header lines before the iteration:
 *   FUN_00006D90("source/monitor/cmd_help.c", 0x2D, "cmd_help", 8);
 *   FUN_00006D90("source/monitor/cmd_help.c", 0x2E, "cmd_help", 8);
 * The format strings these refer to (`Available commands:` /
 * `─────────────────` or similar) live in flash and aren't yet
 * decoded.
 *
 * Skeleton: just iterate the registered help printers; the
 * structured-log emit is stubbed.
 */

#include "bleware.h"

#include <stdint.h>

/* Per-module help printer registration. The OEM keeps this as a
 * static NULL-terminated array at flash 0x0002A0BC. Skeleton:
 * weak-symbol-based registration — each cmd_*.c that has help text
 * provides a `pf_*_help` function and registers it via the linker
 * by being included in the build. */
typedef void (*help_printer_t)(int unused);

extern help_printer_t g_help_printers[];

void cmd_help(void)
{
    /* TODO: emit the two banner lines (logged at file:line
     * cmd_help.c:0x2D and :0x2E in the OEM). The structured-log
     * function isn't yet decomp'd; leave it unstubbed so this
     * compiles. */

    for (help_printer_t *pf = g_help_printers; *pf != 0; pf++) {
        (*pf)(0);
    }
}

/* Skeleton: empty help-printer table. Each cmd_*.c module will
 * append its own printer here as it's decoded. */
__attribute__((weak))
help_printer_t g_help_printers[] = {
    /* (*)(int) → emit-help-block */
    0,  /* NULL-terminator */
};
