#ifndef BLEBOOT_BIM_H
#define BLEBOOT_BIM_H

#include <stdint.h>

/* Cached low 4 bits of the MMIO config word at 0x40032430, shifted
 * left by 10. Written by `main()` once at boot and read by the
 * image-hash function downstream — acts as a per-bike salt for
 * whatever derivation `FUN_000560d8` performs. Defined in main.c. */
extern volatile uint32_t g_hw_id_cached;

/* Top-level boot-decision state machine. Called from `main()` and
 * never returns on the panic branch; falls through to `main`'s
 * `return 0` on the normal-boot branch (which then hits `_exit`). */
void bim_dispatch(void);

/* Image-verify-and-launch — runs on every non-trap dispatch return
 * regardless of the scan's slot pick. Reads the 56-byte OAD header
 * out of flash, checks the magic byte, computes a hash, compares
 * against the header's expected value, writes a 0xFE "verified"
 * marker back into flash if it matches, then jumps to the image's
 * entry point. Returns on any failure. */
void bim_verify_and_launch_image(void);

/* Panic indicator — drives DIO2 high via the GPIO controller's
 * set-only DOUTSET register. Called immediately before the
 * spin-forever in the dispatcher's panic branch so the user sees
 * a status LED come on instead of a silent freeze. */
void bim_panic_indicate(void);

#endif
