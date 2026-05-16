#ifndef BLEBOOT_BIM_H
#define BLEBOOT_BIM_H

#include <stdint.h>

/* Cached chunk-size selector — low 4 bits of MMIO 0x40032430,
 * shifted left by 10 (so the field encodes {1024, 2048, ..., 15360}
 * in 1024-byte steps). Written by `main()` once at boot and read
 * by `bim_crc32_image` as the outer-loop block stride. Defined in
 * main.c. The "hw_id" name used during the first decomp pass was
 * misleading — this is a configuration selector, not an identity. */
extern volatile uint32_t g_oad_chunk_size;

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

/* CRC32-IEEE over an OAD image — the verification primitive used
 * by both `bim_verify_and_launch_image` and
 * `bim_full_scan_and_launch`. Standard reflected polynomial
 * `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR
 * `0xFFFFFFFF`. The first 12 bytes of the image are skipped (those
 * carry the OAD identifier + length fields which CRC over
 * themselves would change the result on every increment). Reads
 * the image via flash (`FUN_000569e4`) when `use_flash != 0`, or
 * via an alt source (`FUN_000570fa`, likely the OAD reception
 * staging buffer in RAM) when `use_flash == 0`. */
uint32_t bim_crc32_image(uint32_t start_page,
                          uint32_t chunk_size,
                          uint32_t skip_offset_base,
                          uint32_t image_size,
                          uint8_t  use_flash);

/* OAD magic check — returns 1 if the 8 bytes pointed to by `hdr8`
 * match the constant ASCII string "OAD NVM1", else 0. Called by
 * the quick-scan sniff and by `bim_slot_iterator`. The reference
 * string lives at flash `0x000571E8`. */
int oad_magic_match(const uint8_t *hdr8);

/* Byte-identical duplicate of `oad_magic_match` at flash
 * `0x00056F98`, referencing the adjacent "OAD NVM1" copy at flash
 * `0x000571E0`. Exists because two source-file translation units
 * each emitted their own static-inlined memcmp + their own
 * private copy of the constant; the linker kept all four pieces.
 * Called only from `bim_slot_iterator`, alongside
 * `oad_magic_match`. Semantically a no-op alias. */
int oad_magic_match2(const uint8_t *hdr8);

/* Flash-session begin — universal precheck at every BIM entry
 * point that touches flash. Returns 1 if the flash subsystem is
 * ready (caller proceeds), 0 if not (caller bails). Also lights
 * DIO3 + DIO4 as a "flash busy" indicator on the BLE PCB. The
 * complementary "release" call is `bim_flash_release`, which
 * every flash-using path invokes after the operation completes
 * (and which `bim_flash_prepare` itself invokes on the
 * probe-failure branch). */
int bim_flash_prepare(void);

/* Flash-session end — clears the flash-op LED, runs the flash
 * controller drain loop, issues a "session closing" marker write,
 * and tears down the PRCM clock state that `bim_flash_prepare`
 * brought up. Called by every flash-using BIM path; treated as
 * fire-and-forget (no status return). */
void bim_flash_release(void);

/* PRCM peripheral + power-domain teardown. Final step of
 * `bim_flash_release`: reconfigures peripheral clocks for two
 * bitmasks (`0x100`, `0x500`) bracketed by the CLKLOADCTL kick,
 * then powers off PRCM domain mask `6` (SERIAL | PERIPH) with a
 * status-poll retry loop. Uses the same ROM dispatch slot at
 * `0x100001B8` as `bim_panic_prep`. Not called from outside
 * `bim_flash_release` in this build. */
void bim_periph_power_off(void);

/* DIO4 set / clear — fine-grained "flash op in flight"
 * indicator on the BLE PCB. Bracket every individual flash MMIO
 * access across the BIM (~13 set sites, ~8 clear sites). Distinct
 * from DIO3 (the coarser "flash session active" LED that
 * `bim_flash_prepare` lights for the whole session). Both write
 * `1<<4` to the corresponding GPIO `DOUT{SET,CLR}31_0`
 * register. */
void dio4_set(void);
void dio4_clear(void);

/* SSI0 + PRCM bring-up — configures SSI0 (SPI master at
 * `0x40000000`) for talking to the external SPI NOR flash chip
 * that stages OAD images. Powers on SERIAL+PERIPH PRCM domains,
 * enables two peripheral run masks (`0x500`, `0x100`), resets
 * SSI0 interrupt state, runs `SSIConfigSetExpClk`-equivalent
 * (48 MHz refclk, 8-bit data, `bit_rate` arg), does an IOC/DMA
 * setup ROM call with `cfg`, enables SSI0, and drains stale
 * RX. Called once at the start of every BIM flash session via
 * `bim_flash_prepare(4_000_000, 9)`. */
void bim_ssi_init(uint32_t bit_rate, uint32_t cfg);

/* SPI flash "Deep Power Down" — sends the JEDEC standard `0xB9`
 * opcode to the external SPI NOR flash, telling it to enter
 * low-power sleep. Issued by `bim_flash_release` as the first
 * teardown step. Standard opcode across Winbond W25Q, Micron
 * N25Q, Macronix MX25, etc. */
void bim_spi_deep_power_down(void);

/* SSI busy-wait, bounded to 10 polls. Loops calling the SSI
 * idle-probe `FUN_0005698C` until it returns 0 (idle), giving
 * up after 10 attempts. Called by `bim_flash_release` after
 * `bim_spi_deep_power_down` to drain in-flight SSI activity
 * before peripheral teardown. */
void bim_spi_wait_idle(void);

/* Image launcher — terminal handoff for all three OAD scan
 * paths. Reloads SP and PC from `*(entry + 4)` (both registers
 * receive the same word — see oad.c commentary on the OEM's
 * off-by-one inline-asm quirk) and `blx`-es into the launched
 * image's Reset_Handler. Treated as returning in case the
 * launch is staged; in this build all real launches are
 * terminal. */
void bim_launch_image(uint32_t entry);

/* Slot iterator — finds the next slot (4 KB stride) starting at
 * `start_slot` whose first 8 bytes match "OAD NVM1". Returns the
 * slot index (0..43) on a hit, `~1` (-2) once `slot` advances past
 * 43 without a hit, or `-1` on the "duplicate match" branch (dead
 * code in this build). Drives `bim_full_scan_and_launch`'s outer
 * loop. */
int bim_slot_iterator(int start_slot);

#endif
