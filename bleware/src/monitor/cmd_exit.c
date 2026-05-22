/* monitor/cmd_exit.c — pseudo-`exit` shell command.
 *
 * OEM entry: 0x00014838 (`cmd_exit`).
 *
 * This isn't a real shell exit — the bleware MCU has no parent
 * process to return to. Instead the handler emits a fixed sequence
 * of bytes that the host-side terminal driver listens for as a
 * "detach now" signal:
 *
 *     0x1B  0x5B  0x31  0x34  0x7E       (= Esc [ 1 4 ~ — the F4 keycode)
 *
 * The host terminal sees the F4 code and closes the debug-console
 * session itself.
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_INFO 8

static const char K_FILE[] = "source/monitor/cmd_exit.c";
static const char K_F4_RAW[] = { 0x1B, 0x5B, 0x31, 0x34, 0x7E, 0x00 };

int cmd_exit(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "exit", 0x05);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("exit", "exit from shell");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "exit") == 0) {
        return 2;
    }

    /* The OEM emits five consecutive monitor_log lines, one per byte
     * of the F4 keycode. Preserved verbatim so the binary diff stays
     * stable on this region. */
    monitor_log(K_FILE, 0x28, "cmd_exit", LOG_LEVEL_INFO, K_F4_RAW, 0x1B);
    monitor_log(K_FILE, 0x29, "cmd_exit", LOG_LEVEL_INFO, K_F4_RAW, 0x5B);
    monitor_log(K_FILE, 0x2a, "cmd_exit", LOG_LEVEL_INFO, K_F4_RAW, 0x31);
    monitor_log(K_FILE, 0x2b, "cmd_exit", LOG_LEVEL_INFO, K_F4_RAW, 0x34);
    monitor_log(K_FILE, 0x2c, "cmd_exit", LOG_LEVEL_INFO, K_F4_RAW, 0x7E);
    return 0;
}
