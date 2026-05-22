/* monitor/table.c — decoded prefix of the static command table.
 *
 * OEM table starts at flash 0x0002A0BC. The full table has 25 entries
 * before the first NULL; this skeleton lists handlers as their source
 * translations land.
 */

#include "monitor.h"

int cmd_firmware_update(int verb, void *p2, void *p3, uint32_t p4);
int cmd_extflash_verify(int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_dump(int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_inject(int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_list(int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_delete(int verb, void *p2, void *p3, uint32_t p4);
int cmd_ble_info(int verb, void *p2, void *p3, uint32_t p4);
int cmd_info_ver(int verb, void *p2, void *p3, uint32_t p4);
int cmd_help(int verb, void *p2, void *p3, uint32_t p4);

const monitor_cmd_handler_t g_monitor_commands[] = {
    cmd_firmware_update, /* OEM 0x0001A968 */
    cmd_extflash_verify, /* OEM 0x0000ABD8 */
    cmd_log_dump,        /* OEM 0x0000C2D4 */
    cmd_log_inject,      /* OEM 0x0000E190 */
    cmd_pack_list,       /* OEM 0x00011D4C */
    cmd_pack_delete,     /* OEM 0x00010554 */
    cmd_ble_info,        /* OEM 0x00007A58 */
    cmd_info_ver,        /* OEM 0x0001B440 */
    cmd_help,            /* OEM 0x00013BE8 */
    0,
};
