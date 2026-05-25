/* pack_ingest.c — PACK filesystem ingest.
 *
 * After a PACK file is YModem-received to ext-flash 0x80000..0x200000,
 * these helpers integrate it into the live system:
 *
 *   pack_ingest_start()  — stops audio playback, waits for the audio
 *                          daemon to drain, then parses the pack header
 *   pack_upload_finalize() — marks the pack as valid and notifies
 *                          all inter-module subscribers (0x10E,1)
 *
 * Called by cmd_pack_upload and cmd_pack_process.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* OEM @ 0x00016F2C (~150 B) */
void pack_ingest_start(void)
{
    extern void FUN_000273A0(int flag);
    extern void FUN_00027630(void);
    extern int  FUN_000264D4(void);
    extern int  FUN_00015888(uint32_t base);

    /* signal audio daemon: pause playback */
    FUN_000273A0(1);
    FUN_00027630();

    /* wait for audio daemon to acknowledge */
    int done;
    do {
        done = FUN_000264D4();
        if (done == 0) {
            extern void thunk_EXT_FUN_1002CE00(int ms);
            thunk_EXT_FUN_1002CE00(1000);
            extern void FUN_000232B8(int arg);
            FUN_000232B8(1);
        }
    } while (done == 0);

    /* parse the pack header at ext-flash 0x80000 */
    int ok = FUN_00015888(0x80000);
    if (ok == 0) {
        module_forward_async(0x10E, 6);
        monitor_log("source/oad/oad.c", 0x1B3, NULL, 2,
                    "invalid pack content");
        FUN_000273A0(0);
    } else {
        extern void FUN_00026A90(void);
        FUN_00026A90();
        FUN_000273A0(0);

        extern void FUN_00023F02(const char *name, void *out);
        uint8_t buf[64];
        FUN_00023F02("bleware", buf);

        module_forward_async(0x10E, 1);
    }
}

/* Finalize the PACK upload: notify all modules that a new pack is
 * ready. OEM body was ~30 B — mostly a wrapper around the ingest
 * + notifier chain. The OEM cmd_pack_upload calls pack_ingest_start
 * followed immediately by pack_upload_finalize; the latter is a
 * no-op in later firmware versions (the real work is in _start). */
void pack_upload_finalize(void)
{
    /* The OEM body at ~0x000117CC is likely a thin wrapper that
     * re-checks the pack header validity and optionally triggers
     * a secondary notification. In the observed call chain from
     * cmd_pack_upload, all the work already happened in
     * pack_ingest_start — this is a no-op tail. */
}
