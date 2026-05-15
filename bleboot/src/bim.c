#include "bim.h"

/* Three helpers still undecoded as of this commit. The return
 * value of FUN_00056254 is the bus that drives the rest of the
 * dispatcher: zero triggers the fallback-prepare path,
 * negative-one triggers the panic path, anything else just falls
 * through. Names mirror Ghidra's still-`FUN_*` symbols so a future
 * decomp pass can drop in real names without grep-replace
 * collisions. The other two helpers
 * (`bim_verify_and_launch_image`, `bim_panic_indicate`) are decoded
 * and declared in `bim.h`. */
extern int  FUN_00056254(void);  /* returns 0, -1, or a slot index */
extern void FUN_00056824(int);   /* called with r0=0 when scan returned 0 */
extern void FUN_00056B64(void);  /* panic cleanup */

void bim_dispatch(void)
{
    int slot = FUN_00056254();

    if (slot == 0) {
        FUN_00056824(0);
    }

    bim_verify_and_launch_image();

    if (slot == -1) {
        FUN_00056B64();
        bim_panic_indicate();
        for (;;) {
        }
    }
}
