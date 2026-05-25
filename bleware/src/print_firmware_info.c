/* cmd_info.c — firmware info printer.
 *
 * Called by cmd_info_ver (monitor command "info" / "ver"). Prints:
 *   - Device name  ("ES3-" + BLE MAC)
 *   - BLE MAC address
 *   - Firmware version, compile date/time
 *   - BIM bootloader version + compile date (if present)
 *   - Last reset reason
 *   - System tick count
 *
 * OEM @ 0x000054D8 (~220 B).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

#define FCFG1_MAC_BLE  0x500012E8u

static const char k_src_file[] = "source/monitor/cmd_info.c";

void print_firmware_info(void)
{
    const uint8_t *mac = (const uint8_t *)FCFG1_MAC_BLE;
    char           name[20];
    uint32_t       logger_tag;

    /* device name: "ES3-" + 6-byte BLE MAC in hex */
    extern int monitor_snprintf(char *buf, unsigned int size,
                                const char *fmt, ...);
    monitor_snprintf(name, sizeof name,
                     "%c%c%c%c%02X%02X%02X%02X%02X%02X",
                     'E', 'S', '3', '-',
                     mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

    logger_tag = 0;
    monitor_log(k_src_file, 0x2A, (void *)logger_tag, 9,
                "BLE MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
                mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);

    monitor_log(k_src_file, 0x2C, (void *)logger_tag, 9, "");

    monitor_log(k_src_file, 0x2D, (void *)logger_tag, 9,
                "Device name:                     %s", name);

    monitor_log(k_src_file, 0x2E, (void *)logger_tag, 9,
                "Firmware version:                %d.%d.%d", 1, 4, 1);

    monitor_log(k_src_file, 0x2F, (void *)logger_tag, 9,
                "Compile date & time:             MMM DD YYYY HH:MM:SS");

    /* BIM bootloader info */
    extern uint32_t *g_bim_info_ptr;   /* DAT_00005840 = 0x20005B24 */
    uint32_t        *bim_hdr = g_bim_info_ptr;
    int              line;
    const char      *bim_fmt;

    if (bim_hdr != NULL && *(uint32_t *)bim_hdr == 0x52455642u /* "BVER" LE */) {
        uint8_t *bim = (uint8_t *)*bim_hdr;
        monitor_log(k_src_file, 0x51, (void *)logger_tag, 9,
                    "BIM firmware version:            %d.%d.%d",
                    bim[0x19], bim[0x1A], bim[0x1B]);
        line    = 0x52;
        bim_fmt = "BIM compile date & time:         %s";
    } else {
        monitor_log(k_src_file, 0x56, (void *)logger_tag, 1,
                    "This is a really old bike with a legacy bootloader");
        line    = 0x57;
        bim_fmt = "BIM firmware version:         %s";
    }
    monitor_log(k_src_file, line, (void *)logger_tag, 9, bim_fmt, 0);

    const char *reason = reset_reason_string();
    monitor_log(k_src_file, 0x5A, (void *)logger_tag, 9,
                "reset type:                      %s", reason);

    extern uint32_t bios_get_systick(void);
    uint32_t ticks = bios_get_systick();
    monitor_log(k_src_file, 0x5C, (void *)logger_tag, 9,
                "systick:                         %u", ticks);
}
