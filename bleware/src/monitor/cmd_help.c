/* monitor/cmd_help.c — monitor `help` command handler.
 *
 * OEM @ 0x00013BE8. Universal cmd_* ABI entry in the command table at
 * 0x0002A0BC. Besides printing its own help row, EXECUTE logs the help
 * banner and then walks the same command table with verb 0.
 */

#include "bleware.h"
#include "monitor.h"

#include <stdint.h>

extern const monitor_cmd_handler_t g_monitor_commands[];

static const char K_FILE[] = "source/monitor/cmd_help.c";

int cmd_help(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "help", 5);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("help", "show all monitor commands");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "help") == 0) {
        return 2;
    }

    monitor_log(K_FILE, 0x2d, "cmd_help", 8,
                "The following commands are available:\r\n");
    monitor_log(K_FILE, 0x2e, "cmd_help", 8, "\r\n");

    const monitor_cmd_handler_t *cursor = g_monitor_commands;
    while (*cursor != 0) {
        (*cursor)(MON_CMD_PRINT_HELP, p2, 0, 0);
        cursor++;
    }

    return 0;
}
