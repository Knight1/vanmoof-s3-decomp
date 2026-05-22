/* monitor/cmd_update.c — firmware-update monitor command.
 *
 * OEM @ 0x0001A968. Command-table entry 0 in the registry at 0x0002A0BC.
 */

#include "monitor.h"

#include <stdint.h>

extern void firmware_update_start(void);

int cmd_firmware_update(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "firmware_update", 0x10);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("firmware_update",
                                "update a new image of firmware through OAD");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "firmware_update") == 0) {
        return 2;
    }

    firmware_update_start();
    return 0;
}
