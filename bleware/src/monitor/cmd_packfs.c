/* monitor/cmd_packfs.c — PACK filesystem monitor commands.
 *
 * OEM entries translated here:
 *   0x00011D4C  pack_list
 *   0x00010554  pack_delete
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_HELP 8

static const char K_FILE[] = "source/monitor/cmd_packfs.c";

extern void *packfs_open(uint32_t base);
extern void *packfs_next(void *ctx);
extern void  packfs_close(void *ctx);
extern const char *format_size(uint32_t bytes, char *dst);
extern int   extflash_open(void *handle);
extern void  extflash_close(void);
extern void  extflash_erase(uint32_t address, uint32_t size);

int cmd_pack_list(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "pack_list", 10);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("pack_list", "list the contents of a PACK file");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "pack_list") == 0) {
        return 2;
    }

    void *ctx = packfs_open(0x80000);
    if (ctx != 0) {
        char size_buf[256];
        monitor_log(K_FILE, 0x66, "cmd_pack_list", LOG_LEVEL_HELP,
                    "Scanning PACK archive...");
        for (;;) {
            uint8_t *entry = (uint8_t *)packfs_next(ctx);
            if (entry == 0) {
                break;
            }
            const char *size = format_size(*(uint32_t *)(entry + 0x3c),
                                           size_buf);
            monitor_log(K_FILE, 0x68, "cmd_pack_list", LOG_LEVEL_HELP,
                        "%14s bytes %s", size, entry);
        }
        packfs_close(ctx);
    }

    return 0;
}

int cmd_pack_delete(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "pack_delete", 12);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("pack_delete", "delete a PACK file");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "pack_delete") == 0) {
        return 2;
    }

    monitor_log(K_FILE, 0x8a, "cmd_pack_delete", LOG_LEVEL_HELP,
                "Deleting PACK archive...");

    if (extflash_open(0) != 0) {
        uint32_t address = 0x80000;
        for (uint32_t i = 0; i < 0x180; i++) {
            monitor_log(K_FILE, 0x91, "cmd_pack_delete", LOG_LEVEL_HELP,
                        "Erase pack progress <%d%%>", (i * 100u) / 0x180u);
            monitor_sleep(4);
            extflash_erase(address, 0x1000);
            address += 0x1000;
        }
        extflash_close();
    }

    monitor_log(K_FILE, 0x99, "cmd_pack_delete", LOG_LEVEL_HELP, "Done");
    return 0;
}
