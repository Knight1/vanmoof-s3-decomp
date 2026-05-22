/* monitor/cmd_ble.c — BLE monitor commands.
 *
 * OEM entries translated here:
 *   0x00007A58  ble_info
 *   0x0001B3C4  ble_disconnect
 *   0x0001E8B8  ble_erase_all_bonds
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

/* GAP terminate-link helper. `conn_handle == 0xFFFD` is the TI BLE-stack
 * "all connections" sentinel (`LINKDB_CONNHANDLE_ALL`); reason code 4
 * = HCI "remote user terminated connection". OEM @ 0x00021030. */
extern int ble_gap_terminate_link(uint16_t conn_handle, uint8_t reason);

/* General 1-byte "post control event" dispatcher. OAD uses codes
 * 0x12..0x17; `ble_erase_all_bonds` uses code 0x20. Decoded as
 * `bleware_control_event_post` in src/oad.c. */
extern void bleware_control_event_post(uint32_t code);

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

int cmd_ble_disconnect(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "ble_disconnect", 0x0f);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("ble_disconnect",
                                "force a disconnect of all connections");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "ble_disconnect") == 0) {
        return 2;
    }

    ble_gap_terminate_link(0xFFFD, 4);
    return 0;
}

int cmd_ble_erase_all_bonds(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "ble_erase_all_bonds", 0x14);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("ble_erase_all_bonds",
                                "erase all bonds");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "ble_erase_all_bonds") == 0) {
        return 2;
    }

    /* Control code 0x20 is consumed by the BLE-stack thread's bonds
     * subsystem. The handler itself does no work — it just posts the
     * code and returns; the actual GAPBondMgr_SetParameter call lives
     * elsewhere. */
    bleware_control_event_post(0x20);
    return 0;
}
