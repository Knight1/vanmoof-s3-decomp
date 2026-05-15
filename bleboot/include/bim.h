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

/* Full scan — first-boot path. Walks every slot the iterator
 * surfaces, fully verifies each candidate (primary CRC, secondary
 * CRC, hash with hw-id salt), promotes the first match by writing
 * a `0xFE` marker into the slot and the image header in flash, and
 * launches it via the image launcher. Returns 0 after walking every
 * slot (or after a successful launch returns), -1 if the initial
 * precheck failed. */
int bim_full_scan_and_launch(void);

/* Quick scan — subsequent-boot fast path. Walks slots 0..43 (8 KB
 * stride) and launches the first slot whose status byte is already
 * `0xFE` (promoted) or `0xFF` (pristine), skipping re-verification.
 * Called from bim_dispatch when the full scan returned 0 without
 * launching. */
void bim_quick_scan_and_launch(int start_slot);

/* Image-verify-and-launch — last-ditch attempt that reads the BIM's
 * own 56-byte OAD header, hashes it, and launches `hdr[28]` if the
 * hash matches `hdr[8]`. Returns on any failure. */
void bim_verify_and_launch_image(void);

/* Panic preparation — performs a 3-step ROM-API handshake, enables
 * the GPIO peripheral clock through PRCM, and switches DIO2 to
 * output mode. Paired with `bim_panic_indicate` to drive the panic
 * LED high right before the spin-forever. */
void bim_panic_prep(void);

/* Panic indicator — drives DIO2 high via the GPIO controller's
 * set-only DOUTSET register. */
void bim_panic_indicate(void);

#endif
