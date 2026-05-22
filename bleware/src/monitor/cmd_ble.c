/* monitor/cmd_ble.c — BLE monitor commands.
 *
 * OEM @ 0x00007A58 for `ble_info`.
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_INFO 9

static const char K_FILE[] = "source/monitor/cmd_ble.c";

extern int      ble_connection_count(int unused);
extern int      ble_connection_present(int index);
extern int      ble_connection_is_rider_app(int index);
extern void     ble_connection_addr(int index, uint8_t *dst);
extern void     ble_connection_params(int index, uint16_t *interval,
                                      uint16_t *latency, uint16_t *timeout);
extern uint8_t *ble_device_address(int addr_type);

int cmd_ble_info(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "ble_info", 9);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("ble_info", "dump current BLE connection info");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "ble_info") == 0) {
        return 2;
    }

    int count = ble_connection_count(1);
    monitor_log(K_FILE, 0x3e, "cmd_ble_info", LOG_LEVEL_INFO,
                "number of connections: %d/%d", count, 3);

    for (int i = 0; i < 3; i++) {
        if (ble_connection_present(i) != 0) {
            uint16_t interval = 0;
            uint16_t latency = 0;
            uint16_t timeout = 0;
            uint8_t addr[10];

            ble_connection_addr(i, addr);
            ble_connection_params(i, &interval, &latency, &timeout);

            monitor_log(K_FILE, 0x45, "cmd_ble_info", LOG_LEVEL_INFO,
                        "conn %d", i);
            monitor_log(K_FILE, 0x47, "cmd_ble_info", LOG_LEVEL_INFO,
                        "Connection type rider app: %c",
                        ble_connection_is_rider_app(i) == 0 ? 'N' : 'Y');
            monitor_log(K_FILE, 0x4b, "cmd_ble_info", LOG_LEVEL_INFO,
                        "Connection timeout: %d", timeout);
            monitor_log(K_FILE, 0x4c, "cmd_ble_info", LOG_LEVEL_INFO,
                        "Connection latency: %d", latency);
            monitor_log(K_FILE, 0x4d, "cmd_ble_info", LOG_LEVEL_INFO,
                        "Connection interval: %d", interval);
            monitor_log(K_FILE, 0x52, 0, LOG_LEVEL_INFO, 0,
                        "BD addr", "%02x:%02x:%02x:%02x:%02x:%02x",
                        addr, 6);
            monitor_log(K_FILE, 0x53, "cmd_ble_info", LOG_LEVEL_INFO, "");
        }
    }

    monitor_log(K_FILE, 0x57, 0, LOG_LEVEL_INFO, 0,
                "Device address", "%02x:%02x:%02x:%02x:%02x:%02x",
                ble_device_address(1), 6);
    return 0;
}
