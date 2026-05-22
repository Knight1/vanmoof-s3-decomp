/* monitor/cmd_info.c — firmware-info monitor commands.
 *
 * OEM @ 0x0001B440 for the `info/ver` table entry.
 */

#include "monitor.h"

#include <stdint.h>

extern void print_firmware_info(void);

int cmd_info_ver(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "info/ver", 9);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("info/ver", "show basic firmware info");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if ((monitor_command_matches((const char *)p2, "info") != 0) ||
        (monitor_command_matches((const char *)p2, "ver") != 0)) {
        print_firmware_info();
        return 0;
    }

    return 2;
}
