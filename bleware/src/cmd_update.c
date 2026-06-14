/* cmd_update.c — firmware update command.
 *
 * Command "firmware_update" (monitor table entry 0). Receives a new
 * firmware image via YModem, validates the OAD-NVM1 header, CRC-checks
 * the image, marks it as pending, and triggers a software reset.
 *
 * OEM @ 0x0000D444 (~180 B).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

static const char k_src_file[] = "source/filetransfer.c";

int firmware_update_start(void *params)
{
    (void)params;
    uint8_t  header[0x2C];
    int      rc;

    /* 1. Receive the firmware image via YModem.
     *    dest = ext-flash 0x00000, max = 0x58000 (352 KB). */
    rc = ymodem_receive(0, 0x58000u);
    if (rc != 0) {
        return -1;
    }

    /* 2. Open ext-flash and read the OAD-NVM1 header. */
    if (extflash_open(0) != 1) {
        return -1;
    }
    extflash_read(0, sizeof header, header);
    extflash_close();

    /* 3. Verify the OAD header magic: "OAD NVM1" (8 bytes). */
    extern const uint8_t g_oad_header_magic[8];  /* DAT_0000D5C8 → "OAD NVM1" */
    if (memcmp(header, g_oad_header_magic, 8) != 0) {
        monitor_log(k_src_file, 0xF0, NULL, 2,
                    "Invalid image header during firmware update");
        return -1;
    }

    /* 4. CRC-validate the received image against the header's
     *    expected CRC field. FUN_00016E7C computes the CRC over
     *    the image bytes; FUN_0000D750 likely returns the stored
     *    expected CRC from the header. */
    extern int FUN_00016E7C(int start_off, uint32_t length, int unused);
    uint32_t   header_len = *(uint32_t *)(header + 0x18);          /* OEM sp+0x24 */
    uint32_t   expected   = *(uint32_t *)(header + 0x08);          /* header.crc32, OEM sp+0x14 */
    uint32_t   computed   = FUN_00016E7C(0xC, header_len - 0xC, 1);

    if (expected != computed) {
        monitor_log(k_src_file, 0xFC, NULL, 2,
                    "CRC failure. Calculated CRC 0x%08X, expected 0x%08X",
                    computed, expected);
        return -1;
    }

    /* 5. Success — delay, mark the OAD image pending-commit, then reset. */
    extern void FUN_00027542(int units);     /* delay */
    extern void FUN_0001BF04(int img_cp_stat);   /* set OAD imgCpStat */

    FUN_00027542(500);
    monitor_log(k_src_file, 0x101, NULL, 8,
                "Firmware upload successful. Preparing software update.");

    /* Set the OAD image's imgCpStat to 0xFE ("pending commit") so the BIM
     * promotes it on the next boot. Load-bearing — without this call the
     * received image is never applied. OEM @ 0x0000D4B8: movs r0,#0xfe;
     * bl 0x0001BF04. */
    FUN_0001BF04(0xFE);

    monitor_log(k_src_file, 0x106, NULL, 0,
                "Trigger software update");

    firmware_abort();
    /* not reached */
    return 0;
}
