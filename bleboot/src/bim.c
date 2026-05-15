#include "bim.h"

/* Top-level boot decision. Dispatcher contract:
 *
 *   bim_full_scan_and_launch()  →  -1  → precheck failed → panic
 *                              →   0   → fall through to quick scan
 *
 * On success, neither scan returns (control is transferred to the
 * launched image via FUN_00057156). When both scans fail to launch,
 * `bim_verify_and_launch_image` is the last-ditch attempt to boot
 * the BIM's own header before the panic path lights DIO2.
 *
 * The OEM dispatcher only checks the precheck failure and the
 * "did the full scan return 0" case, so we preserve that exact
 * decision shape: any other return value would route to the
 * "anything else → continue" branch, which in this build is dead
 * code (bim_full_scan_and_launch only returns 0 or -1). */
void bim_dispatch(void)
{
    int scan_result = bim_full_scan_and_launch();

    if (scan_result == 0) {
        bim_quick_scan_and_launch(0);
    }

    bim_verify_and_launch_image();

    if (scan_result == -1) {
        bim_panic_prep();
        bim_panic_indicate();
        for (;;) {
        }
    }
}
