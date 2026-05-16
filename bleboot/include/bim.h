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
 * the image via flash (`bim_spi_flash_read` + `bim_iflash_read_paged`)
 * when `use_flash != 0`, or via an alt source (`bim_memcpy_safe`,
 * likely the OAD reception staging buffer in RAM) when
 * `use_flash == 0`. */
uint32_t bim_crc32_image(uint32_t start_page,
                          uint32_t chunk_size,
                          uint32_t skip_offset_base,
                          uint32_t image_size,
                          uint8_t  use_flash);

/* Flat-buffer CRC32-IEEE over a contiguous flash range — the
 * sibling of `bim_crc32_image`, used by `bim_full_scan_and_launch`
 * as the primary integrity check against `hdr[8]` before the
 * paged variant runs. Same per-byte polynomial step
 * (`crc32_ieee_byte_step`), but reads bytes sequentially from
 * `addr..addr+len` instead of walking pages of `chunk_size`.
 * `use_spi != 0` reads through `bim_spi_flash_read` (and bounds
 * `len` against the matched chip's capacity from
 * `bim_get_chip_entry`); `use_spi == 0` reads through the alt
 * source `bim_memcpy_safe` (dead in this build). Returns the final
 * CRC value on success, `0` on any failure (zero/maxed length,
 * length exceeds chip capacity). */
uint32_t bim_crc32_buffer(uint32_t addr, uint32_t len, uint8_t use_spi);

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

/* SSI busy-wait, bounded to 10 polls. Loops calling
 * `bim_spi_probe_chip` until it returns 0 (chip not
 * recognized), giving up after 10 attempts. Called by
 * `bim_flash_release` after `bim_spi_deep_power_down` to
 * verify DPD took effect — once the chip stops responding to
 * the probe, we know it has powered down. */
void bim_spi_wait_idle(void);

/* SPI full-duplex transmit — sends `n` bytes from `src` over
 * SSI0 via the SSI ROM table slots [1] (`SSIDataPut`) and [3]
 * (`SSIDataGet`). The received bytes (one per sent byte by
 * SPI's nature) are discarded; this helper is for
 * command-only transmissions like JEDEC opcodes. Returns 0.
 * Sole in-source callers: `bim_spi_deep_power_down` and
 * `bim_spi_release_from_dpd`. */
int bim_spi_send_bytes(const void *src, uint32_t n);

/* SPI full-duplex receive — clocks out `n` dummy `0x00` bytes
 * via SSI ROM slot [2] (`SSIDataPutNonBlocking`) and stores
 * each byte the slave shifts back into `dst[0..n)` via slot
 * [3] (`SSIDataGet`). Returns 0 on success, -1 if any
 * non-blocking put returned 0 (TX FIFO full — defensive). */
int bim_spi_recv_bytes(void *dst, uint32_t n);

/* SPI flash REMS (Read Electronic Manufacturer & Device ID)
 * — sends the 4-byte command `[0x90, 0xFF, 0xFF, 0x00]`
 * (opcode + 24-bit dummy address; LSB=0 → mfr first) over
 * SPI, then receives 2 bytes into the SRAM globals
 * `g_chip_id_byte1` / `g_chip_id_byte2` (= `0x20000404` /
 * `0x20000405`). Returns 1 on success, 0 on any SSI error.
 * Sole caller: `bim_spi_probe_chip`. The BIM uses the
 * REMS opcode `0x90` (2-byte response: mfr+device) rather
 * than the longer JEDEC `0x9F` (3-byte: mfr+type+capacity)
 * because the chip-database table is keyed on REMS. */
int bim_spi_read_rems_id(void);

/* SPI chip-database lookup. Calls `bim_spi_read_rems_id` to
 * fill `g_chip_id_byte1` / `g_chip_id_byte2`, then walks the
 * BIM's table of known SPI flash chips at flash
 * `0x000571A8` (8-byte entries with REMS-signature bytes at
 * offsets [4]/[5], terminated by an 8-byte entry whose
 * first word is `0`). Returns 1 if the connected chip is
 * recognized, 0 otherwise. Called by `bim_flash_prepare`
 * (success gate) and by `bim_spi_wait_idle` (loop
 * predicate). */
int bim_spi_probe_chip(void);

/* SPI flash "wait for Write-In-Progress to clear" — polls
 * the chip's status register (RDSR opcode `0x05`) in a tight
 * loop and returns once bit 0 (WIP) is observed clear.
 * Returns 0 on ready, -2 on SSI receive error. Called as a
 * gate before reads (`bim_spi_flash_read`) and as the
 * post-DPD-wake verification (`bim_spi_release_from_dpd`).
 * **Unbounded loop** — relies on the chip eventually
 * reporting idle. */
int bim_spi_wait_wip(void);

/* SPI flash "Release from Deep Power Down" — sends JEDEC
 * `0xAB` opcode (the inverse of `0xB9` that
 * `bim_spi_deep_power_down` sends), waits ~12 µs for the
 * chip's tRES1 wake-up window, then verifies the chip is
 * alive via `bim_spi_wait_wip` (RDSR until WIP clears).
 * Returns 1 on success, 0 on send or verify failure.
 * Called by `bim_flash_prepare` after `bim_ssi_init`. */
int bim_spi_release_from_dpd(void);

/* SPI flash sequential read — sends a 4-byte READ command
 * (`0x03` + 24-bit big-endian address) over SPI, then
 * receives `len` bytes into `dst`. Returns 1 on success, 0
 * on any error path (wait-WIP fail, send fail, recv fail).
 * Gated on `bim_spi_wait_wip` first. The 24-bit address
 * limits this primitive to the first 16 MB of the chip;
 * the installed MX25L51245G's upper 48 MB is unreachable
 * by this opcode (would need the 4-byte-address READ4B
 * `0x13`, not present in the BIM). */
int bim_spi_flash_read(uint32_t addr, uint32_t len, void *dst);

/* SPI flash "Write Enable" — sends the JEDEC standard `0x06`
 * opcode (loaded from flash literal at `0x000571F5`).
 * Returns 0 on success, -3 on send error. SPI NOR chips
 * require this before every program/erase to arm the WEL
 * latch. Sole caller: `bim_spi_flash_program`. */
int bim_spi_write_enable(void);

/* SPI flash page-program — writes `len` bytes from `src` to
 * the external SPI NOR flash starting at `addr`. Splits the
 * write at 256-byte page boundaries, issuing a fresh `WREN +
 * PP (0x02) + addr + data` sequence per chunk. Returns 1 on
 * success, 0 on any error. Caller must ensure target bytes
 * are pre-erased (`0xFF`); typically used for status-marker
 * writes that flip individual bits `1`→`0` (`0xFF` →
 * `0xFE`/`0xFC`). 24-bit address limit (first 16 MB) — same
 * as `bim_spi_flash_read`. Sole caller:
 * `bim_full_scan_and_launch`. */
int bim_spi_flash_program(uint32_t addr, uint32_t len, const void *src);

/* Internal CC2642 flash program — writes `count` bytes from
 * `src` to internal flash at address `(slot << 13) + offset`.
 * 8 KB per slot matches the CC2642R1F erase-page size, so
 * each slot is one erasable page in the 344 KB bleware region
 * (slots 0..43 → addresses 0..0x55FFF). Returns 0 on success,
 * `0xFF` on program failure (matches the pre-erased flash
 * byte value). Used by the OAD scan paths to drop status
 * markers into the executable image's own page after CRC
 * verification — complement to `bim_spi_flash_program` which
 * writes the corresponding marker to the OAD staging slot on
 * external SPI flash. */
int bim_iflash_program(uint32_t slot, uint32_t offset,
                       const void *src, uint32_t count);

/* External-to-internal flash copy — the OAD promote primitive.
 * Pulls `length` bytes from external SPI flash at `spi_src` and
 * writes them to internal CC2642 flash at `iflash_dst` in
 * 256-byte chunks. Pre-checks that `iflash_dst` is 4-byte
 * aligned, that the touched-page range fits within the bleware
 * region (≤ 44 pages × 8 KB = 352 KB), and that the target
 * pages are pre-erased (`0xFF`) — see the "low-byte truncation"
 * quirk in `bim_iflash_check_range_blank`. Returns 0 on success,
 * -1 on any failure (precondition trip, SPI read, or iflash
 * program). Used by `bim_full_scan_and_launch` (post-CRC
 * promote) and `bim_verify_and_launch_image` (staging-to-exec
 * transfer). */
int bim_iflash_copy_from_spi(uint32_t spi_src, uint32_t length, uint32_t iflash_dst);

/* Internal-flash read primitive — reads `len` bytes from
 * memory-mapped internal flash at `src` to RAM at `dst`, wrapped
 * in an IRQ-disabled critical section so a concurrent program/
 * erase from a hypothetical ISR can't corrupt the read. Nested-
 * safe: only re-enables IRQs on exit if they were enabled going
 * in. Returns 0 (always). Used by `bim_quick_scan_and_launch`
 * for 8-byte sniff and 44-byte short-header reads from already-
 * promoted images sitting in internal flash. */
int bim_iflash_read(const void *src, void *dst, uint32_t len);

/* Internal-flash read with paged addressing — sibling of
 * `bim_iflash_read` that takes `(page, offset)` rather than a flat
 * pointer. The 8 KB stride per page matches the CC2642R1F erase-
 * page size. Same IRQ-safe bracket as `bim_iflash_read`. Sole
 * caller: `bim_crc32_image`'s `use_flash != 0` path (dead in this
 * build but preserved). */
int bim_iflash_read_paged(uint32_t page, uint32_t offset,
                          void *dst, uint32_t count);

/* Defensive memcpy with a null-destination guard. Returns 0
 * (i.e. NULL) without touching memory if `dst` is NULL; otherwise
 * copies `count` bytes from `src` to `dst` and returns `dst`.
 * Sole callers: the alt-source paths in `bim_crc32_buffer` and
 * `bim_crc32_image` (both dead in this build). */
void *bim_memcpy_safe(void *dst, const void *src, uint32_t count);

/* Image-segment-table walker — given the base address of an
 * OAD image header on external SPI flash and the header's
 * total length, reads each 12-byte segment descriptor
 * starting at offset `0x2C` and returns the load address
 * (entry's `seg_value` at offset 8) of the first segment
 * whose `seg_type` (byte 0) equals `1` (TI's "contiguous
 * image" segment type). Returns `0xFFFFFFFF` if no matching
 * segment is found, the SPI read fails, or the segment-list
 * sentinel (`seg_len == 0`) is reached. Used by both
 * `bim_verify_and_launch_image` and `bim_full_scan_and_launch`
 * to compute "where does this image actually run from"
 * before checking integrity. */
uint32_t bim_oad_find_image_addr(uint32_t hdr_base, uint32_t hdr_limit);

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
