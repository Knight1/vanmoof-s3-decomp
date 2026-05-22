/* monitor/table.c — decoded static command table.
 *
 * OEM table starts at flash 0x0002A0BC. 25 function pointers + a
 * NULL terminator. The OEM order is preserved verbatim so the binary
 * diff stays meaningful.
 */

#include "monitor.h"

int cmd_firmware_update     (int verb, void *p2, void *p3, uint32_t p4);
int cmd_extflash_verify     (int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_count           (int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_dump            (int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_flush           (int verb, void *p2, void *p3, uint32_t p4);
int cmd_log_inject          (int verb, void *p2, void *p3, uint32_t p4);
int cmd_audio_play          (int verb, void *p2, void *p3, uint32_t p4);
int cmd_audio_stop          (int verb, void *p2, void *p3, uint32_t p4);
int cmd_audio_dump          (int verb, void *p2, void *p3, uint32_t p4);
int cmd_audio_upload        (int verb, void *p2, void *p3, uint32_t p4);
int cmd_audio_volume_set_all(int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_upload         (int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_list           (int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_delete         (int verb, void *p2, void *p3, uint32_t p4);
int cmd_pack_process        (int verb, void *p2, void *p3, uint32_t p4);
int cmd_ble_info            (int verb, void *p2, void *p3, uint32_t p4);
int cmd_ble_disconnect      (int verb, void *p2, void *p3, uint32_t p4);
int cmd_ble_erase_all_bonds (int verb, void *p2, void *p3, uint32_t p4);
int cmd_shutdown            (int verb, void *p2, void *p3, uint32_t p4);
int cmd_rtos_statistics     (int verb, void *p2, void *p3, uint32_t p4);
int cmd_rtos_nvm_compact    (int verb, void *p2, void *p3, uint32_t p4);
int cmd_reset               (int verb, void *p2, void *p3, uint32_t p4);
int cmd_info_ver            (int verb, void *p2, void *p3, uint32_t p4);
int cmd_exit                (int verb, void *p2, void *p3, uint32_t p4);
int cmd_help                (int verb, void *p2, void *p3, uint32_t p4);

const monitor_cmd_handler_t g_monitor_commands[] = {
    cmd_firmware_update,      /* OEM 0x0001A968 */
    cmd_extflash_verify,      /* OEM 0x0000ABD8 */
    cmd_log_count,            /* OEM 0x0001699C */
    cmd_log_dump,             /* OEM 0x0000C2D4 */
    cmd_log_flush,            /* OEM 0x00016DCC */
    cmd_log_inject,           /* OEM 0x0000E190 */
    cmd_audio_play,           /* OEM 0x0000F19C */
    cmd_audio_stop,           /* OEM 0x0001CE3C */
    cmd_audio_dump,           /* OEM 0x0001522C */
    cmd_audio_upload,         /* OEM 0x0000CAE0 */
    cmd_audio_volume_set_all, /* OEM 0x0000C614 */
    cmd_pack_upload,          /* OEM 0x0001406C */
    cmd_pack_list,            /* OEM 0x00011D4C */
    cmd_pack_delete,          /* OEM 0x00010554 */
    cmd_pack_process,         /* OEM 0x000116AC */
    cmd_ble_info,             /* OEM 0x00007A58 */
    cmd_ble_disconnect,       /* OEM 0x0001B3C4 */
    cmd_ble_erase_all_bonds,  /* OEM 0x0001E8B8 */
    cmd_shutdown,             /* OEM 0x0001DCD0 */
    cmd_rtos_statistics,      /* OEM 0x0000F6A0 */
    cmd_rtos_nvm_compact,     /* OEM 0x00010078 */
    cmd_reset,                /* OEM 0x0001D8E4 */
    cmd_info_ver,             /* OEM 0x0001B440 */
    cmd_exit,                 /* OEM 0x00014838 */
    cmd_help,                 /* OEM 0x00013BE8 */
    0,
};
