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

/* YModem receive: receive into `dst`, accepting up to `max_size`
 * bytes; returns 0 on success, non-zero on protocol/CRC/abort. OEM
 * FUN_000101B0. */
extern int   ymodem_receive(uint32_t dst, uint32_t max_size);

/* Async forward to the inter-module Modbus bus — same helper used
 * across the GATT layer (declared in bleware.h). */
extern int   module_forward_async(uint32_t cmd_id, uint8_t arg);

/* Begin PACK ingest from the staged region at ext-flash 0x80000.
 * OEM FUN_00016F2C. Called both by cmd_pack_upload (after YModem
 * completion) and cmd_pack_process (directly, for an already-staged
 * payload). */
extern void  pack_ingest_start(void);

/* Late-stage commit hook called after a successful YModem PACK
 * upload — flushes any in-progress state. OEM FUN_00027478. */
extern void  pack_upload_finalize(void);

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

int cmd_pack_upload(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "pack_upload", 0x0c);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("pack_upload",
                                "upload a PACK file by Y-Modem");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "pack_upload") == 0) {
        return 2;
    }

    /* `0x10E` is the bike-wide PACK-coordination Modbus channel:
     *   payload 0 = "PACK upload begin"
     *   payload 4 = "PACK upload abort"
     * Modules listening on this id (motorware, mainware) freeze any
     * background access to ext-flash slot 0x80000..0x200000 for the
     * duration. */
    module_forward_async(0x10E, 0);
    int rc = ymodem_receive(0x80000, 0x180000);
    if (rc == 0) {
        pack_ingest_start();
        pack_upload_finalize();
    } else {
        monitor_log(K_FILE, 0x39, "cmd_pack_upload", LOG_LEVEL_HELP,
                    "Received invalid file, abort OAD");
        module_forward_async(0x10E, 4);
    }
    return 0;
}

int cmd_pack_process(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "pack_process", 0x0d);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("pack_process",
                                "process pack files in external flash");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "pack_process") == 0) {
        return 2;
    }

    pack_ingest_start();
    monitor_log(K_FILE, 0xba, "cmd_pack_process", LOG_LEVEL_HELP + 1,
                "Processing pakfs, expect a small wait...");
    return 0;
}
