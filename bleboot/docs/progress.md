# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
42 decomp-c / 2 vendor-stock / 8 named / 11 pending
```

(`63` total functions in Ghidra at base `0x00056000`.)

**Major insight (recorded once, applies project-wide):** the BIM
talks to an **external SPI NOR flash chip via SSI0** — the
internal CC2642 flash holds only the BIM itself (this last 8 KB
page); candidate OAD images live on the external SPI flash. This
is the TI OAD "external flash" build configuration. The `bim_flash_*`
naming is preserved (the OAD images do live on this flash), but
the underlying mechanism is SPI. Evidence:
1. `bim_ssi_init` (`0x00056A88`'s `FUN_000563C8`) configures
   SSI0 at `0x40000000` for SPI master mode via `SSIConfigSetExpClk`-
   equivalent at 4 MHz bit rate, 8-bit data.
2. `bim_spi_deep_power_down` sends the JEDEC standard `0xB9`
   opcode (Deep Power Down) — a command meaningful only to
   external SPI NOR flash chips.
3. The 48 MHz literal (`0x02DC6C00`) inside `bim_ssi_init`
   matches CC2642R1F's HF XOSC frequency, fed as the refclk to
   the SSI clock divider.

**Identified chip and pin assignment (Apr 2026 pass):**

- **DIO4 is the SPI flash chip-select** (`/CS`, active low),
  bit-banged manually via `dio4_clear` (assert) /
  `dio4_set` (release) brackets around every SPI transaction.
  Confirmed by the bracket pattern in `bim_spi_flash_read`,
  `bim_spi_read_rems_id`, `bim_spi_deep_power_down`, and
  `bim_spi_release_from_dpd`. The earlier "DIO4 = flash op
  in flight LED" interpretation was wrong; the function names
  are kept (`dio4_set` / `dio4_clear` are accurate at the
  MMIO level) but the semantic is /CS, not an LED.
- **Installed flash chip: Macronix MX25L51245GMI-08G-TR**
  (512 Mbit / 64 MB, SOIC-8, 2.7–3.6 V). Identified from the
  PCB; matches entry 0 of the chip-database table at flash
  `0x000571A8`. The BIM uses the legacy REMS opcode `0x90`
  (returns 2 bytes: mfr `0xC2`, device `0x19`) rather than
  full JEDEC `0x9F` (3 bytes including capacity), so the
  table is keyed only on the (mfr, device) pair.
- **Chip-database table** at flash `0x000571A8`, 8-byte
  entries, NULL-terminated. Five known chips supported by
  the BIM, presumably reflecting VanMoof's BLE PCB design
  history:

| word[0] (capacity B) | mfr  | dev  | Chip identification | Notes |
| --- | --- | --- | --- | --- |
| `0x04000000` (64 MB)  | `C2` | `19` | Macronix MX25L51245G (512 Mbit) | **installed** on this PCB |
| `0x00200000` (2 MB)   | `C2` | `15` | Macronix MX25L1606 (16 Mbit)    | older / smaller variant |
| `0x00100000` (1 MB)   | `C2` | `14` | Macronix MX25L8006 (8 Mbit)     | older / smaller variant |
| `0x00080000` (512 KB) | `EF` | `12` | Winbond W25X40 (4 Mbit)         | older / smaller variant |
| `0x00040000` (256 KB) | `EF` | `11` | Winbond W25X20 (2 Mbit)         | older / smaller variant |
| `0x00000000` (term)   | —    | —    | end-of-table sentinel           | first word == 0 |

  Implication: at 64 MB, the installed chip is **way larger**
  than what the BIM actually addresses — it only ships the
  3-byte-address READ opcode (`0x03` in `bim_spi_flash_read`),
  which limits reachability to the first 16 MB. The remaining
  48 MB is unreachable by the BIM. Either VanMoof oversized
  the chip for future-proofing, or the BIM was never updated
  after a chip swap, or downstream code (in `bleware`, not
  here) uses the upper bank for some other purpose (BLE
  pairing data, app NV storage, log buffer).

**Toolchain identified**: the OEM image was built with the **TI ARM
Compiler / CCS**, not GCC. Evidence: the `_auto_init_*` descriptor-
table runtime init at `0x56BF0` (50 B walking 8-byte `{type, arg}`
records through a function-pointer dispatch table) is TI CGT's
canonical compiler-runtime pattern — GCC emits inline-asm
.data/.bss copy loops instead. Also confirmed by the
`_system_pre_init` weak hook at `0x571A0` and the BIM project
layout in the TI SDK shipping CCS / IAR variants.

## Per-module log

- **`rts_hooks.c`** — Two TI CCS RTS weak-default hooks the OEM
  image kept unchanged: `_system_pre_init` at `0x000571A0`
  (returns 1 to enable cinit; compiles to `movs r0, #1; bx lr` =
  `2001 4770`) and `_exit` at `0x000571A4` (post-main trap;
  compiles to `nop; b .` = `bf00 e7fe`). Both are byte-equivalent
  to the OEM under the project CFLAGS.
- **`bim.c`** — `bim_dispatch` at `0x00056F2A`. 38 B (OEM) / 34 B
  (ours, `-Os`) — behaviour-equivalent, not byte-equivalent.
  Three-way fan-out on the return of `bim_full_scan_and_launch`:
  scan returns `0` → call `bim_quick_scan_and_launch(0)` (the lazy
  fast path); scan returns `-1` → panic path (`bim_panic_prep`,
  `bim_panic_indicate`, then spin); anything else would just
  continue, but this build's full scan never returns other values
  so the third branch is unreachable in practice. The always-run
  successor `bim_verify_and_launch_image` runs in every non-trap
  case. The OEM divergence is GCC noticing it can replace
  `adds r4,r0,#0; bne` with a `cbnz r0` (no need for the explicit
  `movs r0,#0` on the fall-through, since r0 is already 0 there),
  and `cmp.w r4,#-1; bne` with `adds r4,#1; bne` (mutates r4 but
  it's dead after) — saving 4 B total.
- **`flash.c`** — Now hosts 23 functions covering the full
  external SPI flash session and the internal CC2642 flash
  program path, end-to-end: session begin/end
  (`bim_flash_prepare`, `bim_flash_release`), PRCM teardown
  (`bim_periph_power_off`), SSI0 bring-up (`bim_ssi_init`),
  DPD bracket (`bim_spi_deep_power_down`, `bim_spi_release_from_dpd`),
  chip identification (`bim_spi_read_rems_id`,
  `bim_spi_probe_chip`), SPI primitives (`bim_spi_send_bytes`,
  `bim_spi_recv_bytes`, `bim_ssi_rx_drain`), status polling
  (`bim_spi_wait_wip`), the leaf SPI flash read / write-enable /
  page-program primitives (`bim_spi_flash_read`,
  `bim_spi_write_enable`, `bim_spi_flash_program`), DPD-landed
  verification (`bim_spi_wait_idle`), the two /CS toggle leaves
  (`dio4_set`, `dio4_clear` — confirmed to drive the SPI
  flash chip-select line, not an indicator LED), and the
  internal-flash program wrapper (`bim_iflash_program`)
  delegating to three ROM-API helpers
  (`bim_iflash_session_begin`,
  `bim_iflash_program_via_rom`,
  `bim_iflash_session_end` — all renamed in Ghidra,
  C-translation pending). The /CS pair is widely shared
  across the flash subsystem: `dio4_set` has 13 call sites,
  `dio4_clear` has 8.

  **The OAD promote primitive stack** the
  function that actually copies a verified candidate from
  external SPI staging to its internal-flash executable
  destination is `bim_iflash_copy_from_spi` (`0x00056714`,
  140 B), called from both `bim_full_scan_and_launch` (after
  CRC) and `bim_verify_and_launch_image`. It pulls 256-byte
  chunks via `bim_spi_flash_read` and writes them via
  `bim_iflash_program_flat` (`0x00056F00`, 42 B — a flat-
  address sibling of `bim_iflash_program`). Pre-erase
  verification happens via `bim_iflash_check_range_blank`
  (`0x00056C34`) which iterates pages via
  `bim_iflash_check_slot_blank` (`0x00056FBC`), in turn
  calling the TI ROM-API blank-check helper
  `bim_iflash_rom_blank_check` (`0x00057058`). The
  `bim_iflash_check_range_blank` routine has an OEM low-byte
  truncation quirk that effectively makes the check "are
  slots 0..N blank?" rather than "are the actual target slots
  blank?" — preserved verbatim; documented in `flash.c` and
  the function-table row.

  **The IRQ-safe internal-flash read** (added May 2026):
  `bim_iflash_read` (`0x00056E40`, 50 B) wraps a memcpy from
  memory-mapped internal flash to RAM in a PRIMASK
  save/restore via the leaf pair `bim_irq_disable_save`
  (`0x00057164`) / `bim_irq_enable_restore` (`0x00057170`).
  Nested-safe — only re-enables IRQs on exit if they were
  enabled going in. Used by `bim_quick_scan_and_launch` for
  the 8-byte sniff and 44-byte short-header reads of
  already-promoted images sitting in internal flash.
- **`bim_ssi_init` (`0x000563C8`)** — 172 B in OEM. Behaviour-
  equivalent reconstruction. The big "what is this BIM really
  doing" function. Three external resources brought up in
  sequence:
  1. **PRCM** (via ROM slot `0x100001B8`, same as bring-up's
     mirror in `bim_periph_power_off`): power on SERIAL+PERIPH
     domains, enable peripheral run masks `0x500` and `0x100`,
     each followed by the CLKLOADCTL kick-and-wait.
  2. **SSI0** at `0x40000000`: reset interrupt state (clear
     low 4 bits of `IM`, write 3 to `ICR`), configure clock and
     protocol via the SSI ROM slot at `0x100001C4` slot [0]
     (= `SSIConfigSetExpClk` with refclk 48 MHz, bit rate
     4 MHz, 8-bit data, mode 0 / SPI), enable via `CR1.SSE`,
     drain RX via slot [4] in a loop.
  3. **IOC/DMA** via the third ROM slot at `0x100001B4`,
     slot [17], with args `(SSI0_BASE, 6, 5, -1, cfg)`. Pin
     routing for the SSI0 MISO/MOSI/SCLK/CSn lines is the
     most likely interpretation.
  Caller (`bim_flash_prepare`) passes `(4_000_000, 9)`: 4 MHz
  SPI clock + IOC config word 9.
- **`bim_spi_deep_power_down` (`0x000570C8`)** — 26 B. Sends
  the JEDEC standard `0xB9` opcode to the external SPI NOR
  flash, telling it to enter low-power sleep. Sole caller:
  `bim_flash_release` (first step). The `0xB9` byte lives on
  the stack — pushed in the `r3` slot of the prologue's
  `push {r3, lr}` (a TI CCS technique for 1-byte stack
  scratch). Brackets the 1-byte SSI write (via `bim_spi_send_bytes`)
  with `dio4_clear` / `dio4_set` — the only place in the BIM
  where DIO4 is briefly *clear* during work; everywhere else
  DIO4 is held high across sub-ops.
- **`bim_spi_wait_idle` (`0x000570E2`)** — 24 B. Bounded
  polling loop: calls `bim_spi_probe_chip` up to 10 times,
  exiting early when it returns 0. With the probe now
  understood, the loop's real role becomes clearer: it waits
  until the SPI flash chip stops being recognized (= DPD took
  effect). Called from `bim_flash_release` immediately after
  `bim_spi_deep_power_down` as a verify-DPD-landed step. The
  OEM's loop uses a `uint8_t` counter (emits `uxtb` on
  increment), suggesting the source was
  `for (uint8_t i = 0; i < 10; i++) {...}`.
- **`bim_spi_send_bytes` (`0x00056EA4`)** — 44 B. SPI
  full-duplex transmit primitive. For each byte: `SSIDataPut`
  (slot [1]) then `SSIDataGet` (slot [3]) on SSI0. The
  received bytes are discarded — this helper is for
  command-only transmissions (JEDEC opcodes). Returns 0
  unconditionally. Sole callers: `bim_spi_deep_power_down`
  (n=1, opcode `0xB9`) and `bim_spi_release_from_dpd` (n=1,
  opcode `0xAB`). Behaviour-equivalent reconstruction
  (register allocation differs).
- **`bim_spi_probe_chip` (`0x0005698C`)** — 74 B. Calls
  `bim_spi_read_rems_id` (which sends the REMS opcode `0x90`
  and stashes the 2-byte response at SRAM
  `0x20000404`/`0x20000405`), then walks the 8-byte-stride
  table at flash `0x000571A8` searching for an entry whose
  signature bytes at offsets [4]/[5] match the readout. The
  walk-cursor lives in SRAM at `0x20000408` (reset on every
  call). Returns 1 if a matching non-NULL entry was found, 0
  if the read failed or the table exhausted. Used by
  `bim_flash_prepare` (success gate — only proceed if the
  external SPI flash is recognized) and `bim_spi_wait_idle`
  (loop predicate). The 8-byte entry layout has a non-NULL
  first word (= chip-specific parameter pointer), signature
  bytes at [4] and [5] (= JEDEC manufacturer + device ID),
  and 2 bytes of padding/flags at [6]/[7]. The TI BIM
  source equivalent is `extFlashInfo`.
- **`bim_spi_release_from_dpd` (`0x00056D6A`)** — 56 B. Three
  steps: (1) send JEDEC `0xAB` (Release from Deep Power Down)
  via `bim_spi_send_bytes`, bracketed with /CS toggle;
  (2) tRES1 wake-up delay via a 200-iter `uint16_t` spin loop
  (~12 µs at 48 MHz HF — within JEDEC's 3–30 µs spec window);
  (3) verify the chip via `bim_spi_wait_wip` (RDSR poll).
  Returns 1 on success. Sole caller: `bim_flash_prepare` as
  the gate between SSI bring-up and the chip-probe tail. The
  "send failed → fail" branch is dead in this build
  (`bim_spi_send_bytes` always returns 0) but preserved
  verbatim. The C reconstruction uses a `volatile uint16_t`
  counter for the delay; GCC will emit a different loop shape
  than the OEM's `mov r0, r1; subs r1, r0, #1; cmp r0, #0;
  bne` pattern but the iteration count and total delay are
  preserved.
- **`bim_spi_recv_bytes` (`0x00056C78`)** — 58 B. SPI
  full-duplex receive primitive. For each byte: clock out a
  dummy `0x00` via SSI ROM slot [2] (`SSIDataPutNonBlocking`
  — note non-blocking; returns 0 if the TX FIFO was full),
  then receive 1 byte via slot [3] (`SSIDataGet`, blocking)
  into a 1-byte stack scratch, store at `dst[i++]`. Returns
  0 on success, -1 if any TX put returned 0 (defensive — the
  4-byte FIFO and serial put-then-get loop should keep this
  unreachable). Counterpart to `bim_spi_send_bytes` (which
  uses slot [1] = `SSIDataPut`, blocking, with TX byte from
  the source buffer instead of zero). The two primitives
  together form the BIM's core SPI master interface.
- **`bim_ssi_rx_drain` (`0x00056FE0`)** — 28 B. Standalone
  RX-FIFO drain helper: loops calling SSI ROM slot [4]
  (`SSIDataGetNonBlocking`) on SSI0 until it returns 0
  (FIFO empty). Sole in-source caller: `bim_spi_wait_wip`'s
  prep pulse, used to clear any stale RX bytes left over
  from a previously interrupted operation before the polling
  loop starts. Distinct from the inlined drain at the tail
  of `bim_ssi_init` (semantically identical, separately
  emitted by the TI compiler — same one-source-multiple-
  copies pattern as the `oad_magic_match` / `oad_magic_match2`
  duplication).
- **`bim_spi_read_rems_id` (`0x00056CF4`)** — 52 B. Sends the
  4-byte REMS command word `[0x90, 0xFF, 0xFF, 0x00]`
  (loaded from flash literal at `0x000571F0`) over SPI,
  then receives 2 bytes into the SRAM globals at
  `0x20000404`/`0x20000405` (`g_chip_id_byte1` /
  `g_chip_id_byte2`). Returns 1 on success, 0 on send/recv
  failure. Bracketed with `dio4_clear` / `dio4_set` (i.e.
  /CS asserted across the 4-byte send + 2-byte recv as a
  single SPI transaction). Sole caller:
  `bim_spi_probe_chip`. **Note**: opcode `0x90` is the
  legacy REMS (Read Electronic Manufacturer & Device ID),
  which returns 2 bytes (mfr + device); the table at
  `0x000571A8` is keyed on this 2-byte pair. The longer
  JEDEC ID `0x9F` (3 bytes including capacity) would have
  needed a different table layout; the BIM's choice of
  REMS is consistent with the chip-database-as-lookup-table
  design.
- **`bim_spi_wait_wip` (`0x00056AD4`)** — 68 B. SPI flash
  "wait for Write-In-Progress to clear" — RDSR polling
  loop. Two phases: (1) **prep pulse**: assert /CS, drain
  any stale RX bytes via `bim_ssi_rx_drain`, release /CS;
  (2) **polling loop**: each iteration assert /CS, send
  RDSR opcode `0x05` (loaded once from flash literal at
  `0x000571F4`), recv 1 status byte, release /CS, exit if
  bit 0 (WIP) is clear. Returns 0 on ready, -2 on recv
  error. Loop is **unbounded** — relies on the chip
  eventually reporting idle. Used by
  `bim_spi_release_from_dpd` (post-wake verification) and
  `bim_spi_flash_read` (gate before every read). The 1-byte
  RDSR opcode buffer lives in the `r1` slot of the
  prologue's `push {r1, r2, r3, lr}` — same TI CCS
  1-byte-stack-scratch pattern that
  `bim_spi_deep_power_down` and `bim_spi_release_from_dpd`
  use for their 1-byte opcode buffers.
- **`bim_spi_flash_read` (`0x000569E4`)** — 84 B. SPI flash
  sequential read primitive. The leaf consumed by every
  BIM caller that pulls bytes off the external SPI flash:
  the slot iterator's 8-byte sniff (`bim_slot_iterator`),
  the verify-and-launch 56-byte header read, the full-scan
  path's metadata reads, and (in this build, dead)
  `bim_crc32_image`'s flash-source path. Steps:
  (1) gate on `bim_spi_wait_wip`; (2) build the 4-byte SPI
  READ command on the stack `[0x03, addr_hi, addr_mid,
  addr_lo]` (opcode `0x03` + 24-bit big-endian address);
  (3) assert /CS, send 4-byte cmd; (4) recv `len` bytes
  into `dst` via `bim_spi_recv_bytes`; (5) release /CS.
  Returns 1 on success, 0 on any error. **24-bit addressing
  limits this primitive to the first 16 MB of the chip**
  — the upper 48 MB of the installed MX25L51245G is
  unreachable by this opcode (would need the 4-byte-address
  READ4B `0x13`, not present in the BIM). Either the OAD
  slots are confined to the first 16 MB, or the upper
  bank is owned by `bleware` for some other purpose
  (BLE pairing, app NV, log buffer). **ABI fix (Apr 2026):**
  earlier decomp passes claimed signature `(addr, dst, len)`
  — the actual OEM ABI is `(addr, len, dst)`. The existing
  oad.c / crc.c call sites had it right; only the C
  reconstruction was wrong. Now corrected.
- **`bim_spi_write_enable` (`0x00056ED4`)** — 24 B + 4 B
  literal. Sends the JEDEC standard `0x06` opcode (loaded
  from flash literal at `0x000571F5`, byte 1 of the shared
  opcode word at `0x000571F4`). Brackets the 1-byte SSI
  send with `dio4_clear` / `dio4_set`. Returns 0 on
  success, -3 on send error. SPI NOR chips require WREN
  before every program/erase/write-status to arm the WEL
  (Write Enable Latch); WEL clears automatically after
  each program/erase. Sole in-source caller:
  `bim_spi_flash_program`. **Earlier note correction**:
  byte 1 of the literal was previously called "dead in
  this build" — wrong, this is the caller.
- **`bim_spi_flash_program` (`0x000567A0`)** — 132 B.
  External SPI flash page-program. Writes `len` bytes from
  `src` to `addr`, splitting the write at every 256-byte
  page boundary. Per chunk: `bim_spi_wait_wip` →
  `bim_spi_write_enable` → build PP cmd `[0x02, addr_hi,
  addr_mid, addr_lo]` (24-bit big-endian addressing,
  same 16 MB limit as `bim_spi_flash_read`) → assert /CS
  → send 4-byte cmd → send chunk data → release /CS.
  Returns 1 on success, 0 on any error. Caller must
  ensure target bytes are pre-erased (`0xFF`); typically
  used for status-marker writes that flip individual
  bits `1`→`0` (e.g. `0xFF` → `0xFE` for "verified",
  `0xFF` → `0xFC` for "rejected"). Does NOT wait for the
  program to complete before returning — the next
  caller's `bim_spi_wait_wip` handles that. Sole caller:
  `bim_full_scan_and_launch` (4 sites — transient `0xFC`
  and final `0xFE` markers on the OAD staging slots in
  external SPI flash).
- **`bim_iflash_program` (`0x00056E72`)** — 50 B. Internal
  CC2642 flash program. Writes `count` bytes from `src` to
  internal flash at address `(slot << 13) + offset`.
  8 KB stride per slot matches the CC2642R1F erase-page
  size, so each slot is one erasable page in the 344 KB
  bleware region (slots 0..43 → `0x00000000..0x00055FFF`).
  Three steps, all delegated to ROM-API helpers:
  (1) `bim_iflash_session_begin` (`FUN_00056E0C`) brings
  up the internal flash controller, returns an opaque
  "previous state" handle for the matching tear-down;
  (2) `bim_iflash_program_via_rom` (`FUN_0005703C`) calls
  ROM table at `0x100001A8` slot [6] (TI's
  `FlashProgram(src, dst, count)` driverlib equivalent)
  and clears MMIO `0x42600484` (a status-clear or
  VIMS-side gate); (3) `bim_iflash_session_end`
  (`FUN_00057090`) tears down the session iff bring-up
  changed state. Returns 0 on success, `0xFF` on
  failure (matches the pre-erased flash byte value —
  "byte didn't take" reads back as `0xFF`). Used by
  the OAD scan paths to drop status markers into the
  executable image's own internal-flash page —
  complement to `bim_spi_flash_program` which writes
  the corresponding marker to the OAD staging slot on
  external SPI flash. Callers:
  `bim_full_scan_and_launch` (1 site, `slot+17` marker),
  `bim_verify_and_launch_image` (1 site, BIM's own
  header marker).
  **Major insight (recorded once):** the BIM uses BOTH
  external SPI flash (OAD staging buffer, 4 KB stride
  per `bim_spi_flash_read` calls in scan/verify) AND
  internal CC2642 flash (executable image storage,
  8 KB stride per `bim_iflash_program` slot index). The
  full-scan promote sequence writes a status marker on
  BOTH (`bim_spi_flash_program` to external,
  `bim_iflash_program` to internal). The earlier
  description of "the BIM keeps a tighter grid for
  metadata" was misleading — the two strides reflect
  different storage media, not different metadata
  granularities.
- **`bim_oad_find_image_addr` (`0x00056CB8`)** — 60 B.
  OAD image-segment-table walker. Given the base
  address of an OAD image header on external SPI flash
  and the header's total length, walks the segment-
  descriptor list (12-byte entries starting at offset
  `0x2C`) looking for a type-1 (contiguous-image)
  segment, returning its `seg_value` (load address).
  TI's `imgHdr_t` segment format: byte 0 = `seg_type`,
  bytes 4..7 = `seg_len` (advance to next entry), bytes
  8..11 = `seg_value` (load address for type 1). Loop
  termination: SPI read failure, type-1 hit, `seg_len
  == 0` sentinel, or offset >= `hdr_limit`. Returns
  `0xFFFFFFFF` on no-match. Used by both
  `bim_verify_and_launch_image` and
  `bim_full_scan_and_launch` to compute the executable
  load address before integrity check. The OEM holds
  the default `-1` return value in `r8` across the
  whole loop and only updates it on a type-1 hit; we
  mirror that with a single result variable. The OEM
  also has a curious `str r0, [sp, #0]; ldmia sp!,
  {r0, ...}` epilogue pattern that ends up restoring
  `r0` from the stored value (no-op effectively); it's
  a TI CCS quirk and not preserved verbatim in our
  reconstruction.
- **`dio4_set` / `dio4_clear` (`0x00057138` / `0x00057188`)** —
  Each is a 4–5 instruction leaf that writes `1<<4` to the
  appropriate GPIO `DOUT{SET,CLR}31_0` register, then `bx lr`.
  DIO4 is the fine-grained "flash op in flight" indicator on
  the BLE PCB, distinct from DIO3 (the coarser "flash session
  active" LED that `bim_flash_prepare` lights for the whole
  session). The asymmetric caller count (13 set / 8 clear)
  suggests some helpers nest the bracket and rely on an outer
  caller for the matching clear.
  
  The OEM's `dio4_set` exhibits a TI CCS literal-pool-sharing
  trick: load `GPIO_DOUTCLR31_0 = 0x400220A0` (the canonical
  pool constant shared across every DOUTCLR call site in this
  image) and subtract `0x10` to derive `GPIO_DOUTSET31_0 =
  0x40022090`. This saves bytes when many functions share the
  pool but doesn't help in isolation. GCC doesn't replicate the
  trick — it loads `0x40022090` directly with its own per-
  function literal — so our `dio4_set` is 4 instructions
  (8 B + 4 B literal = 12 B) vs the OEM's 5 instructions (10 B
  + 6 B shared pool = 16 B in this image). `dio4_clear` matches
  more closely: same 4-instruction shape, only the register
  allocation differs (OEM picks `r0`/`r1`, GCC picks `r2`/`r3`).
  
  Both required `__attribute__((noinline))` to preserve the OEM
  call-graph — GCC otherwise inlined them into
  `bim_flash_prepare` (and presumably into every other caller),
  collapsing the `bl dio4_set` semantics into raw DOUTSET
  writes.
- **`bim_periph_power_off` (`0x00056A38`)** — 68 B + 12 B literal
  pool in both OEM and ours. Three literals: ROM dispatch slot
  `0x100001B8` and the PRCM CLKLOADCTL pair (`0x60082028`
  write-trigger alias, `0x40082028` read-ack alias). Three
  operations through the PRCM ROM sub-table at `0x100001B8`
  (same slot `bim_panic_prep` uses for the inverse bring-up):
  (1) `prcm_table[8](0x100)` followed by the canonical
  CLKLOADCTL kick-and-wait (`*0x60082028 = 1`, spin until
  `*0x40082028 & 2`); (2) the same idiom with mask `0x500`;
  (3) a retry loop pairing `prcm_table[6](6)` with
  `prcm_table[13](6)`, looping until the status read from [13]
  equals `2`. Mask `6` is `PRCM_DOMAIN_SERIAL | PRCM_DOMAIN_PERIPH`
  in the TI SimpleLink convention; status `2` is the "domain
  ready / off-acked" sentinel. The retry covers the
  off-request-races-in-flight-access case. Notable cross-
  function fact: slot `[13]` of the PRCM sub-table is the
  read/status accessor across both `bim_panic_prep` (arg `4` =
  `PRCM_DOMAIN_PERIPH` alone) and this function (arg `6`), so
  we can confidently identify [13] = `PRCMPowerDomainStatus`-
  equivalent in the TI ROM API ordering. The other slots in
  this build's PRCM table: [5] (panic_prep, power-on), [6]
  (here, power-off), [7] (panic_prep, periph-enable), [8]
  (here, periph-reconfigure). The OEM repeatedly re-derefs
  `*0x100001B8` before each call rather than caching the table
  pointer — kept as-is.
- **`bim_flash_release` (`0x000570AC`)** — 22 B + 4 B literal in
  both OEM and ours, byte-equivalent. Pure wrapper that
  delegates the four teardown steps: (1) `bim_spi_deep_power_down`
  (26 B) — sends JEDEC `0xB9` opcode to the external SPI flash,
  putting it in low-power sleep; (2) `bim_spi_wait_idle`
  (24 B) — 10-iteration drain/poll loop on the SSI status;
  (3) clear DIO4 directly via
  `GPIO_DOUTCLR31_0` (literal at `0x400220A0`, value `1<<4`),
  the flash-op LED that `dio4_set` lit during prepare —
  note DIO3 (the flash-busy LED that prepare lit inline) is
  **not** cleared here, presumably either held lit through
  image launch or cleared inside one of the sub-helpers'
  inner work; (4) `bim_periph_power_off` (68 B) — clock/PRCM
  teardown sequencer that issues several calls through the ROM
  dispatch slot at `0x100001B8` (same slot used by
  `bim_panic_prep`) with modified-immediate args `0x100` and
  `0x500`, brackets each with a busy-wait on a flash-controller
  status word, and finishes with a `cmp #2; bne` retry on the
  final return — the inverse of the PRCM bring-up that
  `bim_ssi_init` does at session start.
- **`bim_flash_prepare` (`0x00056A88`)** — 64 B in
  both OEM and ours (+ 12 B of literal pool in both). Same size,
  behaviour-equivalent. Universal flash-session opener: every BIM
  call that reads or writes flash (`bim_full_scan_and_launch`,
  `bim_verify_and_launch_image`, `bim_crc32_image` in the
  `use_flash` path) calls this first and bails if it returns 0.
  Sequence is: (1) `bim_ssi_init(4_000_000, 9)` — configures
  SSI0 (SPI master at `0x40000000`) for talking to the external
  SPI NOR flash at 4 MHz bit rate, brings up PRCM SERIAL+PERIPH
  domains; (2) two ROM-API dispatch calls via the table at ROM
  `0x100001B4` (4 bytes earlier than `bim_panic_prep`'s
  `0x100001B8`, so an adjacent sub-table in TI's standard
  `ROM_API_TABLE` array) with `index 15`, args `4` then `3` —
  likely SPI flash "Release from Deep Power Down" (`0xAB` JEDEC
  opcode) and a follow-up status/configuration write, mirroring
  release's DPD; (3) light DIO3 (`1<<3` to `GPIO_DOUTSET31_0`) — a
  flash-busy indicator; (4) call `dio4_set`, which lights
  DIO4 (`1<<4` to the same DOUTSET register) — so the
  combination DIO3+DIO4 is the BLE PCB's "flash session active"
  display; (5) two-stage probe `bim_spi_release_from_dpd` then
  `bim_spi_probe_chip`. The OEM's `0x400220A0` literal + `subs r0, #16`
  hints that `0x400220A0` (= `GPIO_DOUTCLR31_0`) is a
  literal-pool-shared constant across many call sites in this
  image; the OEM saves bytes globally by reusing it and adjusting
  per-site. GCC stores the exact `0x40022090` it needs without
  the subtract — different code shape, same byte count overall.
  The complementary "flash session end" routine is
  `bim_flash_release`, called by every flash-using path (and by
  this function on the failure branch).
- **`crc.c`** — Now hosts three functions: `bim_crc32_image` at
  `0x000560D8` (the paged-OAD CRC compute), `bim_crc32_buffer`
  at `0x0005653C` (the flat-buffer CRC compute), and
  `crc32_ieee_byte_step` at `0x00056F50` (the shared per-byte
  polynomial step). Together they implement standard CRC32-IEEE
  (polynomial `0xEDB88320`, init/final XOR `0xFFFFFFFF`) without
  a precomputed table — the step function materialises one table
  entry on demand by 8-bit polynomial division. Trades ~8 cycles
  per byte for ~1 KB of flash savings, which fits the BIM's 8 KB
  budget. The two compute routines share the same `0x20000300`
  256-byte SRAM scratch buffer; safe because the BIM never
  reenters either CRC routine within a single boot.
- **`crc32_ieee_byte_step` (`0x00056F50`)** — 32 B (OEM) / 22 B
  (ours, `-Os`, +4 B literal). Pure textbook 8-iteration loop;
  the polynomial constant in the literal pool is the only thing
  that identifies it. GCC's compiled shape uses
  `and r1, r0, #1; lsrs r0, r0, #1; cbz r1, ...; eors r0, r2`
  where the OEM uses `lsrs r3, r1, #1; bcs/bcc; ...; eor.w r1, r2,
  r1, lsr #1` (carry-flag-driven). Both compute the same CRC.
  The OEM also emits a dead `beq` at the loop entry (`beq.n
  0x56f6c` immediately after `movs r0, #8` — Z is clear, so the
  branch is never taken); suggests the original C source had an
  `if (count == 0) return crc;` guard that the compiler kept
  defensively. Preserved in our source for fidelity, although
  GCC `-Os` strips it.
- **`bim_crc32_image` (`0x000560D8`)** — 376 B (OEM) / 384 B
  (ours, `-Os`) — behaviour-equivalent. **Confirmed**:
  the BIM's "hash" verification is just **CRC32-IEEE** (reflected
  polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR
  `0xFFFFFFFF`) — not a cryptographic hash. The first 12 bytes
  of the image are skipped (the OAD identifier + length fields,
  which can't self-cover). Image bytes are pulled into a 256-byte
  SRAM scratch buffer at `0x20000300` via one of two sources: a
  flash path (`FUN_000569e4` with TI-style precheck via
  `FUN_00056a88`) or an alt path (`FUN_000570fa`, addressed by
  byte offset only — likely the OAD reception staging buffer in
  RAM). Both BIM call sites pass `use_flash = 0`, so the flash
  path is dead code in this build. The outer loop partitions the
  image into chunks of `g_oad_chunk_size` bytes (cached from
  MMIO `0x40032430` at boot); the middle loop iterates 256-byte
  blocks within each chunk; the inner loop runs the textbook
  CRC32 byte step `crc = TABLE[(crc^byte)&0xFF] ^ (crc>>8)`
  via the per-byte polynomial-division helper at `FUN_00056F50`
  (still `pending`). **Security implication**: this verification
  gate provides integrity but not authenticity — any image whose
  bytes produce the matching CRC32 word passes. There's no
  per-bike binding; `g_oad_chunk_size` is a per-board configuration
  selector, not a per-device secret. The earlier "per-bike hash
  salt" interpretation was wrong; that has been corrected through
  the docs and code (renamed `g_hw_id_cached` →
  `g_oad_chunk_size`, `image_hash` → `image_crc`, `image_addr` →
  `image_size`).
- **`oad_magic_match` (`0x00056F74`) + `oad_magic_match2`
  (`0x00056F98`)** — Each 32 B + 4 B literal in both OEM and ours;
  the two are **byte-identical** in the OEM, differing only in
  which copy of the reference string their literal pool points
  at: `oad_magic_match` references `0x000571E8`,
  `oad_magic_match2` references `0x000571E0`. The two strings
  sit back-to-back forming `"OAD NVM1OAD NVM1"` at flash
  `0x000571E0..0x000571EF`. The duplication is a TI CCS artifact
  — two translation units each emitted their own static-inlined
  memcmp plus their own private copy of the constant, and the
  linker kept all four pieces. Both walk 8 byte positions
  high-to-low; the OEM uses an indexed counter (`ldrb [r2, r1]`
  on each side, walking `r1` from 7 down) while GCC rewrites the
  same algorithm as a pointer walk via post-decrement addressing
  modes — a textbook example of how `-Os` rewrites a counted
  loop into a pointer walk when the trip count is compile-time
  constant. The OEM keeps an unreachable `bmi` guard immediately
  after `movs r1, #7` — same defensive shape as
  `crc32_ieee_byte_step`, suggesting the source was a generic
  `memcmp_count`-style helper used at multiple call sites with
  constant and variable counts. In `bim_slot_iterator` the two
  match functions are called back-to-back on the same buffer, so
  the second call's result is determined by the first
  (functionally the second is dead code), but both `bl` sites
  are preserved in our reconstruction for OEM call-graph fidelity.
- **`bim_slot_iterator` (`0x00056B1C`)** — 72 B (OEM) / 62 B
  (ours, `-Os`) — behaviour-equivalent. Walks slots from
  `start_slot` to 43, reads 8 bytes from `(slot << 12)` via
  `FUN_000569E4`, and returns the slot index on a magic match
  or `-2` (`~1`) on exhaustion. The OEM's `-1` return is
  guarded behind `oad_magic_match` (the second of two identical
  checks) and is therefore unreachable in practice; preserved
  in the reconstruction. GCC saves 10 B by eliminating the
  `r5` running-flag local and merging the loop-end test into
  the loop tail via fall-through. The 44-byte stack frame
  (only 8 used for the sniff buffer) is preserved as a 44-byte
  local for OEM fidelity.
- **`oad.c`** — Now hosts four functions:
  `bim_verify_and_launch_image` at `0x000568A8`,
  `bim_quick_scan_and_launch` at `0x00056824`,
  `bim_full_scan_and_launch` at `0x00056254`, and
  `bim_launch_image` at `0x00057156` (the terminal handoff every
  scan path calls once a candidate passes verification). The
  three scanners operate on variants of the same OAD-style image
  header, share the same external helper set (CRC compute, hash
  compute, flash read, flash program, slot iterator), and share
  the same `0xFE`/`0xFC` status convention (`0xFE` = verified
  marker; `0xFC` = rejected; `0xFF` = pristine, accepted by the
  quick path only). The four together form a tiered boot
  decision: full → quick → verify-and-launch → panic, with
  `bim_launch_image` as the exit.
- **`bim_launch_image` (`0x00057156`)** — 14 B in both OEM and
  ours, **byte-equivalent**. Naked function with inline asm; the
  only way to emit the exact bytes (no portable C can write
  `sp`). One curiosity worth flagging: the OEM loads BOTH `sp`
  and `r0` from the same memory location (`*(entry + 4)`), so
  the new stack pointer ends up holding the called function's
  address rather than a sensible stack top. The launched image's
  `Reset_Handler` reloads `sp` from its own vector-table[0] as
  one of its first instructions (standard ARM Cortex-M startup
  convention), so the launcher works by accident. Interpretation
  that fits the byte sequence: the source was a hand-written
  inline-asm sequence intended as `sp = *entry; pc = *(entry+4)`
  but with an off-by-one that made both reads target offset 4.
  The mistake is silent because the called image patches `sp`
  before any stack operation. Preserved verbatim because changing
  it would break image launches that rely on the existing
  contract. The compiled GNU `as` would otherwise pick the T2
  encoding of `adds r0, #4` (`0x3004`) where the OEM has the T1
  encoding (`0x1d00`); we force the latter with a `.short`
  directive to preserve byte equivalence.
- **`bim_verify_and_launch_image` (`0x000568A8`)** — 120 B in both
  OEM and ours — size-equivalent but not byte-equivalent. Reads
  the 56-byte primary OAD header at flash address 0 via
  `bim_spi_flash_read(0, 56, &hdr)`, bails if `hdr.status != 0xFE`,
  computes an image-base word via
  `bim_oad_find_image_addr(0, hdr.image_size)`, then **promotes**
  the staged candidate via
  `bim_iflash_copy_from_spi(0, hdr.image_size, base)` (copies the
  image body from external SPI into its destination in internal
  CC2642 flash, in 256-byte chunks, after pre-erase verification).
  If the copy succeeds, computes the CRC32 over the image body via
  `bim_crc32_image(page, g_oad_chunk_size, 0, hdr.image_size, 0)`.
  On match against `hdr.image_crc`, writes a 1-byte `0xFE` marker
  into internal flash at offset 17 of the image's own page via
  `bim_iflash_program` and jumps to `hdr.entry` via
  `bim_launch_image`. The `0xFC` write to the local `status` byte
  preceding the compare is dead on the mismatch path but
  materialised by `-Os` because the C source has the default
  value inline with the declaration.
- **`bim_quick_scan_and_launch` (`0x00056824`)** — 130 B (OEM) /
  102 B (ours, `-Os`) — behaviour-equivalent. Lazy fast-path
  scanner: walks slots `start_slot..43` (8 KB stride), reads an
  8-byte sniff via `FUN_00056f74` to verify the slot starts with
  what looks like an OAD header, then a 44-byte short header. If
  the flags byte at offset 18 is in `{1, 3, 7}`, the magic bytes
  at offsets 12/13 are `3`/`1`, and the status byte at offset 17
  is `0xFE` (promoted) or `0xFF` (pristine), launches the entry
  word at offset 24 via `FUN_00057156`. Skips on `0xFC` (rejected)
  or any other status. GCC's `-Os` saves 28 B by collapsing the
  flags check `{1,3,7}` into `(f & 0xFD) == 1 || f == 7` and the
  two status branches (`0xFE`, `0xFF`) into a single `cmp r3,
  0xFD; bls skip`.
- **`bim_full_scan_and_launch` (`0x00056254`)** — 366 B (OEM) /
  324 B (ours, `-Os`) — behaviour-equivalent. First-boot /
  post-OAD-update path. Bails immediately with `-1` if
  `FUN_00056a88()` returns 0 (some kind of "is the BIM allowed to
  scan now?" precheck). Otherwise iterates slots via
  `FUN_00056b1c(prev)` (a slot iterator state machine returning
  the next slot or `-1`), with **4 KB stride** for the primary
  header read on **external SPI flash** (`r4<<12`, via
  `bim_spi_flash_read`) and **8 KB stride** for the
  verified-marker write on **internal CC2642 flash** (`r4<<13`,
  via `bim_iflash_program`). The two strides reflect different
  storage media, not different metadata granularities — the
  BIM stages OAD candidates on external SPI at a 4 KB grid
  and promotes them by writing markers to the corresponding
  internal flash 8 KB page (where bleware actually executes).
  For each slot it: (a) primary CRC32 check over the image body
  via `bim_crc32_buffer(slot_base+12, image_size-12, 1)` against
  `hdr[8]` (flat-buffer variant — same per-byte CRC32-IEEE as
  `bim_crc32_image` but reads sequentially via
  `bim_spi_flash_read`, bounded against the matched chip's
  capacity through `bim_get_chip_entry`); (b) on match, reads a
  44-byte secondary header from a fixed `slot_base = 0` anchor (a
  single global metadata buffer, not the slot itself); (c) derives
  the image base via `bim_oad_find_image_addr`, **promotes**
  the candidate from external SPI to internal flash via
  `bim_iflash_copy_from_spi(slot_base, derived_len, image_base)`,
  writes a transient `0xFC` to `slot+16`; (d) computes the paged
  CRC32 over the now-internal image body via
  `bim_crc32_image(page, g_oad_chunk_size, slot_base, derived_len,
  0)` and compares against `hdr2[8]` — this is a re-check after
  promotion to confirm the copy itself wasn't corrupted; (e) on
  final match writes `0xFE` to both the slot's `+17` byte on
  external SPI and the image-page's `+17` byte on internal flash,
  then launches via `bim_launch_image(hdr2[28])` if the flags
  byte at offset 18 is in `{0, 1, 3, 7}`. Mismatches write `0xFC`
  to `slot+17` (reject) and clear the transient `+16` marker.
  Returns 0 after the iterator exhausts all slots (or after a
  successful launch returns, which shouldn't happen in practice
  but the compiler doesn't know that). Note: the `slot_base = 0`
  register is held constant by the OEM in r5 across the entire
  function but never modified — so several flash writes that look
  like they take a slot-relative address (`slot_base + 17`,
  `slot_base + 16`) hit literal flash addresses 16/17 in this
  build. Preserved as-is.
- **`panic.c`** — Two functions: `bim_panic_indicate` at `0x00057194`
  and `bim_panic_prep` at `0x00056B64`. Together they implement
  the BIM's "I'm about to spin forever, please notice" sequence —
  prep configures DIO2 as an output (and quiesces some
  ROM-managed peripheral via the dispatch table at `0x100001B8`),
  then indicate drives the pin high so the user sees an LED come
  on.
- **`bim_panic_indicate` (`0x00057194`)** — 12 B in both OEM and
  ours (8 B code + 4 B literal). Single write of `1<<2` to
  `GPIO_BASE (0x40022000) + DOUTSET31_0 (0x90) = 0x40022090`.
  Behaviour-equivalent but not byte-equivalent: GCC chose
  registers `r2/r3` where TI CCS chose `r0/r1`.
- **`bim_panic_prep` (`0x00056B64`)** — 54 B (OEM) / 56 B (ours,
  `-Os`) — behaviour-equivalent. Three steps: (1) ROM-API call
  `dispatch[5](4)`, (2) busy-wait on `dispatch[13](4) == 1`,
  (3) ROM-API call `dispatch[7](0x500)` — the dispatch table is
  a pointer-to-table-of-fp at ROM address `0x100001B8`, which
  *might* be `ROM_API_TABLE[14]` (UART) in the SimpleLink CC13x2/
  CC26x2 SDK's standard ordering but the argument values
  (`4`, `0x500`) don't fit UART semantics, so the exact API
  identity is left open. After the ROM dance, the function opens
  the GPIO peripheral clock through PRCM by writing `1` to the
  unbuffered alias `0x60082028` and polling bit 1 of
  `0x40082028` (PRCM_GPIOCLKGR's `LOAD_DONE`-style ack), then
  switches DIO2 to output mode via the bit-band alias
  `0x42441A08` (= bit 2 of GPIO_DOE31_0 at `0x400220D0`). The
  +2 B vs OEM comes from GCC using `lsls r3, r3, #30 / bpl`
  where TI CCS used `lsrs r1, r1, #2 / bcc` — both check bit 1
  of the same word, just via different paths.
- **`main.c`** — BIM main at `0x00057000`. Reads low 4 bits of
  the MMIO config register at `0x40032430`, caches `(val << 10)`
  in the SRAM global at `0x20000400`, then tail-calls the (still
  undecoded) BIM dispatcher at `0x56F2A`. Returns 0 to the
  startup. Compiles to 24 B at `-Os`, same size as the OEM, but
  **not byte-equivalent**: GCC reorders `(x & 0xF) << 10` into
  `(x << 10) & 0x3C00`, which costs us one different 32-bit
  encoding even though the count and registers are the same.
  Behaviour is identical. Note: the exact identity of MMIO
  `0x40032430` isn't yet pinned to a named CC2642R1F register
  (it sits in the FLASH / VIMS controller band, `0x40030000..
  0x40034000`); the low 4 bits look like a hardware-revision /
  package code that downstream BIM logic consults to pick an
  application slot.
- **`exception.c`** — Three byte-identical 2-byte `b .` (Thumb
  `0xE7FE`) trap loops at `0x000568A6` / `0x00056C76` / `0x00056DA2`,
  routed from the HardFault, default, and NMI vector slots
  respectively. Compiled back to `e7fe` per handler with the project
  CFLAGS (`-Os -mthumb -mcpu=cortex-m4`); byte-equivalent.
- **Reset path identification (no module file yet)** — The
  Reset_Handler at `0x57126` is the stock TI driverlib `ResetISR`
  pattern, split across two physical entry points:
    * `0x57126` (10 B "stub"): `push {r3,lr}; bl SetupTrimDevice;
      b.w localProgramStart`. The 6 B at `0x57130..0x57135`
      (`bl HardFault_Handler; pop {r3,pc}`) are GCC's defensive
      epilogue — unreachable because `localProgramStart` is a
      tail-call to a `noreturn`-style function.
    * `0x56DD8` (52 B body, here named `ResetISR_body`): loads MSP
      from the literal pool (`0x20014000` — VT[0] / top of SRAM),
      enables the FPU (`SCB->CPACR |= 0xF00000`), runs two `nop`
      barriers, then chains into `.data` copy / `.bss` zero /
      `main` via the helpers at `0x571A0`, `0x56BF0`, `0x57000`,
      and `0x571A4`.
  Matches `source/ti/devices/cc13x2_cc26x2/startup_files/startup_gcc.c`
  in the SimpleLink CC13x2/CC26x2 SDK 3.40.00.02 (April 2020 — the
  TI release closest to bleboot's `Apr 23 2020` build date). No C
  source written yet — deferred until the `0x56DD8` body's helpers
  are decoded so we can either vendor TI's `startup_gcc.c` verbatim
  or hand-write a single `Reset_Handler` whose lowered code matches
  the OEM byte layout (the split entry is the GCC `-Os` artifact of
  compiling a single C-level `ResetISR` with a noreturn tail-call).

## Function table

| Status | Address | Size (B) | Name | Module | Notes |
| --- | --- | --- | --- | --- | --- |
| decomp-c     | `0x000568A6` | 2   | `HardFault_Handler` | `exception.c` | `b .` trap loop, byte-equivalent |
| decomp-c     | `0x00056C76` | 2   | `Default_Handler`   | `exception.c` | `b .` trap loop, byte-equivalent |
| decomp-c     | `0x00056DA2` | 2   | `NMI_Handler`       | `exception.c` | `b .` trap loop, byte-equivalent |
| decomp-c     | `0x000560D8` | 376 | `bim_crc32_image`            | `crc.c`       | CRC32-IEEE over an OAD image. Polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`, first 12 bytes skipped. Uses a 256-byte SRAM scratch at `0x20000300`; reads via flash (dead path in this build) or alt source. Behaviour-equivalent (+8 B vs OEM). |
| decomp-c     | `0x00056A88` | 64  | `bim_flash_prepare`          | `flash.c`     | Universal "flash session begin" precheck. Calls `bim_ssi_init(4_000_000, 9)` to bring up SSI0 + PRCM + IOC, lights DIO3 + DIO4 as a "flash busy" indicator, then runs the SPI flash bring-up: `bim_spi_release_from_dpd` (sends JEDEC `0xAB`) then `bim_spi_probe_chip` (reads JEDEC ID + checks against the known-chip table). Returns 1 if the connected SPI flash is recognized, 0 if not. Same total size as OEM (64 B + 12 B literal pool). |
| decomp-c     | `0x000570AC` | 22  | `bim_flash_release`          | `flash.c`     | Complement to `bim_flash_prepare`: called by every flash-using BIM path after the operation completes (and recursively by `bim_flash_prepare` on probe failure). Four-step teardown: marker-byte write (`FUN_000570C8`), 10-iter drain/poll (`FUN_000570E2`), clear DIO4 via `GPIO_DOUTCLR31_0 = 1<<4`, then PRCM/clock teardown via `bim_periph_power_off`. DIO3 is **not** cleared here — held lit through image launch or cleared inside a sub-helper. 22 B + 4 B literal in both OEM and ours, byte-equivalent. |
| decomp-c     | `0x00056A38` | 68  | `bim_periph_power_off`       | `flash.c`     | PRCM peripheral + power-domain teardown. Final step of `bim_flash_release`. Two `prcm_table[8](mask)` calls (`0x100`, `0x500`) each bracketed by the CLKLOADCTL kick-and-wait (`*0x60082028 = 1`, spin until `*0x40082028 & 2`), then a retry loop pairing `prcm_table[6](6)` with `prcm_table[13](6)` until [13] returns `2`. ROM dispatch via `0x100001B8` — same slot `bim_panic_prep` uses for the inverse bring-up. 68 B + 12 B literal in both OEM and ours. |
| decomp-c     | `0x00057138` | 10  | `dio4_set`                   | `flash.c`     | Writes `1<<4` to `GPIO_DOUTSET31_0` (DIO4 on, "flash op in flight" indicator). 13 call sites across the BIM. OEM uses the literal-pool-sharing trick (`ldr r0, =0x400220A0; subs r0, #0x10`); GCC loads `0x40022090` directly — 4 instructions (12 B) vs OEM 5 (10 B + 6 B shared pool = 16 B). Marked `noinline` to preserve the OEM call graph. Behaviour-equivalent. |
| decomp-c     | `0x00057188` | 10  | `dio4_clear`                 | `flash.c`     | Writes `1<<4` to `GPIO_DOUTCLR31_0` (DIO4 off). 8 call sites. Same 4-instruction shape as OEM (no subs trick needed for the DOUTCLR write); only register allocation differs (OEM `r0`/`r1`, GCC `r2`/`r3`). Marked `noinline`. Behaviour-equivalent. |
| decomp-c     | `0x000563C8` | 172 | `bim_ssi_init`               | `flash.c`     | SSI0 + PRCM bring-up — configures SSI0 (SPI master at `0x40000000`) for the external SPI NOR flash that stages OAD images. Powers on SERIAL+PERIPH domains via PRCM ROM `0x100001B8` (slots [5], [7], [13]), runs `SSIConfigSetExpClk`-equivalent via SSI ROM `0x100001C4` slot [0] (refclk 48 MHz, bit_rate `arg0` = 4 MHz, 8-bit data), IOC/DMA setup via FLASH ROM `0x100001B4` slot [17] with `arg1` = `cfg`. Behaviour-equivalent. |
| decomp-c     | `0x000570C8` | 26  | `bim_spi_deep_power_down`    | `flash.c`     | Sends JEDEC `0xB9` opcode to the external SPI NOR flash (standard "Deep Power Down" command across Winbond/Micron/Macronix). Brackets the 1-byte SSI write (via `bim_spi_send_bytes`) with `dio4_clear` / `dio4_set`. Sole caller: `bim_flash_release`. The `0xB9` lives on the stack in the `r3` slot of the prologue's push frame. |
| decomp-c     | `0x000570E2` | 24  | `bim_spi_wait_idle`          | `flash.c`     | Bounded busy-wait, ≤10 polls of `bim_spi_probe_chip`. Called by `bim_flash_release` after `bim_spi_deep_power_down` to verify DPD took effect (exits when chip stops responding to probes). Source shape: `for (uint8_t i = 0; i < 10; i++)` — OEM emits `uxtb` on increment. Behaviour-equivalent. |
| decomp-c     | `0x00056EA4` | 44  | `bim_spi_send_bytes`         | `flash.c`     | SPI full-duplex transmit: for each byte, `SSIDataPut(SSI0, byte)` then `SSIDataGet(SSI0, &scratch)`. Discards RX. Used by both DPD opcode senders (`0xB9` enter and `0xAB` release). Returns 0. |
| decomp-c     | `0x0005698C` | 74  | `bim_spi_probe_chip`         | `flash.c`     | SPI chip-database lookup. Reads JEDEC ID via `FUN_00056CF4` (pending), then walks an 8-byte-stride table at flash `0x000571A8` searching for an entry whose signature bytes at [4]/[5] match the readout. SRAM globals: cursor at `0x20000408`, JEDEC bytes at `0x20000404`/`0x20000405`. Returns 1 if recognized, 0 otherwise. Equivalent to TI's `extFlashInfo`. |
| decomp-c     | `0x00056D6A` | 56  | `bim_spi_release_from_dpd`   | `flash.c`     | Sends JEDEC `0xAB` (Release from DPD), waits ~12 µs (200-iter spin loop for tRES1), then verifies via `bim_spi_wait_wip` (RDSR poll). Returns 1 on success. Sole caller: `bim_flash_prepare` as the bring-up gate before chip-probe. |
| decomp-c     | `0x00056C78` | 58  | `bim_spi_recv_bytes`         | `flash.c`     | SPI full-duplex receive primitive. For each byte: clock out a dummy `0x00` via SSI ROM slot [2] (`SSIDataPutNonBlocking`), then receive 1 byte via slot [3] (`SSIDataGet`) into `dst[i++]`. Returns 0 on success, -1 if any non-blocking put returned 0 (TX FIFO full — defensive). |
| decomp-c     | `0x00056FE0` | 28  | `bim_ssi_rx_drain`           | `flash.c`     | Loops calling SSI ROM slot [4] (`SSIDataGetNonBlocking`) on SSI0 until it returns 0 (FIFO empty). Sole caller: `bim_spi_wait_wip`'s prep pulse. Distinct from the inlined drain at the tail of `bim_ssi_init` (TI compiler emitted both copies separately). |
| decomp-c     | `0x00056CF4` | 52  | `bim_spi_read_rems_id`       | `flash.c`     | Sends the 4-byte REMS command word `[0x90, 0xFF, 0xFF, 0x00]` (loaded from flash literal at `0x000571F0`) over SPI, then receives 2 bytes into the SRAM globals at `0x20000404`/`0x20000405` (`g_chip_id_byte1`/`g_chip_id_byte2`). Returns 1 on success. Sole caller: `bim_spi_probe_chip`. Uses legacy REMS opcode `0x90` (mfr+device, 2 bytes) rather than full JEDEC `0x9F` (3 bytes), matching the chip-database table layout. |
| decomp-c     | `0x00056AD4` | 68  | `bim_spi_wait_wip`           | `flash.c`     | SPI flash "wait for Write-In-Progress to clear" — RDSR polling loop. Prep pulse: assert /CS, drain stale RX, release /CS. Polling loop: assert /CS, send RDSR opcode `0x05` (loaded from flash literal at `0x000571F4`), recv 1 status byte, release /CS, exit if bit 0 (WIP) is clear. Returns 0 on ready, -2 on recv error. **Unbounded loop** — relies on the chip eventually reporting idle. Called by `bim_spi_release_from_dpd` (post-wake verify) and `bim_spi_flash_read` (gate before every read). |
| decomp-c     | `0x000569E4` | 84  | `bim_spi_flash_read`         | `flash.c`     | SPI flash sequential read primitive — the leaf consumed by every BIM caller that pulls bytes off the external SPI flash. Signature `(addr, len, dst)`. Steps: gate on `bim_spi_wait_wip`; build `[0x03, addr_hi, addr_mid, addr_lo]` (READ opcode + 24-bit big-endian address); assert /CS; send 4-byte cmd; recv `len` bytes via `bim_spi_recv_bytes`; release /CS. Returns 1 on success. **24-bit addressing limits reach to the first 16 MB** of the installed MX25L51245G (64 MB) — upper 48 MB is unreachable by this opcode. |
| decomp-c     | `0x00056ED4` | 24  | `bim_spi_write_enable`       | `flash.c`     | Sends JEDEC `0x06` (WREN) opcode (loaded from flash literal at `0x000571F5`) over SPI, bracketed with `dio4_clear` / `dio4_set`. Returns 0 on success, -3 on send error. SPI NOR chips require WREN before every program/erase to arm the WEL latch. Sole caller: `bim_spi_flash_program`. |
| decomp-c     | `0x000567A0` | 132 | `bim_spi_flash_program`      | `flash.c`     | External SPI flash page-program. Signature `(addr, len, src)`. Splits writes at 256-byte page boundaries; each chunk: `wait_wip` → `write_enable` → build PP cmd `[0x02, addr_hi, addr_mid, addr_lo]` → assert /CS → send 4-byte cmd → send chunk data → release /CS. Returns 1 on success. Caller must ensure target bytes are pre-erased (`0xFF`); used for marker writes that flip individual bits `1`→`0` (`0xFE`/`0xFC`). 24-bit address limit (first 16 MB). Sole caller: `bim_full_scan_and_launch` (4 sites). |
| decomp-c     | `0x00056E72` | 50  | `bim_iflash_program`         | `flash.c`     | Internal CC2642 flash program — writes `count` bytes from `src` to internal flash at `(slot << 13) + offset` (8 KB stride per slot = the CC2642R1F erase-page size). Three-step bracket via ROM-API helpers: `bim_iflash_session_begin` → `bim_iflash_program_via_rom` (TI's `FlashProgram` driverlib equivalent) → `bim_iflash_session_end`. Returns 0 on success, `0xFF` on failure (matches the pre-erased flash byte value). Used by the OAD scan paths to drop `0xFE` markers into the executable image's own internal-flash page — complement to `bim_spi_flash_program` (which writes the corresponding marker to the OAD staging slot on external SPI flash). Callers: `bim_full_scan_and_launch`, `bim_verify_and_launch_image`. |
| decomp-c     | `0x00056CB8` | 60  | `bim_oad_find_image_addr`    | `oad.c`       | OAD image-segment-table walker. Signature `(hdr_base, hdr_limit)`. Walks 12-byte segment-descriptor entries starting at `hdr_base + 0x2C` looking for a type-1 (contiguous-image) segment, returns its load address (`seg_value` at offset 8). Termination: SPI read failure, type-1 hit, `seg_len == 0` sentinel, or `offset >= hdr_limit`. Returns `0xFFFFFFFF` on no-match. Each entry read via `bim_spi_flash_read` into a 12-byte stack buffer. Used by `bim_verify_and_launch_image` and `bim_full_scan_and_launch` to compute the executable load address before integrity check. |
| decomp-c     | `0x0005653C` | 158 | `bim_crc32_buffer`           | `crc.c`       | Flat-buffer CRC32-IEEE compute — sibling of `bim_crc32_image`, same per-byte `crc32_ieee_byte_step` but operating on a single contiguous `[addr, addr+len)` SPI flash range. Bounded against the matched chip's capacity via `bim_get_chip_entry`. Reads 256-byte chunks into the same `0x20000300` scratch buffer that `bim_crc32_image` uses. Returns the final CRC on success, 0 on failure (zero/maxed length, length > chip capacity). Sole caller: `bim_full_scan_and_launch` as the primary `hdr[8]` integrity check before the paged variant runs. |
| decomp-c     | `0x00056714` | 140 | `bim_iflash_copy_from_spi`   | `flash.c`     | External-to-internal flash copy — the **OAD promote primitive**. Signature `(spi_src, length, iflash_dst)`. Pulls `length` bytes from external SPI flash and writes them to internal CC2642 flash in 256-byte chunks via `bim_iflash_program_flat`. Pre-checks: 4-byte alignment on `iflash_dst`, page-range fit ≤ 44 pages × 8 KB = 352 KB, and pre-erased target slots (`bim_iflash_check_range_blank` — note the low-byte-truncation quirk that makes the check effectively "slots 0..N blank?" rather than the actual target slots). Returns 0 on success, -1 on any failure. Callers: `bim_full_scan_and_launch` (post-CRC promote), `bim_verify_and_launch_image` (staging-to-exec transfer). |
| decomp-c     | `0x00056F00` | 42  | `bim_iflash_program_flat`    | `flash.c`     | Flat-address internal flash program — sibling of `bim_iflash_program` with a flat dst address instead of `(slot, offset)`. Same begin/program-via-ROM/end three-step sequence. Returns 0 on success, `0xFF` on failure. Sole caller: `bim_iflash_copy_from_spi` (256-byte chunk programming). |
| decomp-c     | `0x00056C34` | 66  | `bim_iflash_check_range_blank` | `flash.c`   | Internal-flash blank-range verification — iterates pages covering the byte range `[addr_low, addr_low + length)` and confirms each is blank via `bim_iflash_check_slot_blank`. Returns 0 on all-blank, `0xFF` on first non-blank page. **Quirk:** the OEM passes only the low 8 bits of the destination address (`uxtb`), so `start_page = (addr & 0xFF) / chunk_size` collapses to 0 for any reasonable chunk size; the check is effectively "are pages 0..N blank?" regardless of where the write will land. Preserved verbatim. Sole caller: `bim_iflash_copy_from_spi`. |
| decomp-c     | `0x00056FBC` | 32  | `bim_iflash_check_slot_blank` | `flash.c`    | Internal-flash blank-page check, slot-indexed. Wraps `bim_iflash_rom_blank_check(slot << 13)` in a session begin/end bracket. Returns `0xFF` if the 8 KB page is blank (caller continues), 0 if not (caller bails). Sole caller: `bim_iflash_check_range_blank`. |
| decomp-c     | `0x00056E40` | 50  | `bim_iflash_read`            | `flash.c`     | Internal-flash read primitive — memcpy from memory-mapped internal flash (`0x00000000..0x00057FFF`) to RAM, wrapped in an IRQ-disabled critical section via `bim_irq_disable_save` / `bim_irq_enable_restore` (so a concurrent program/erase from an ISR can't corrupt the read). Nested-safe: only re-enables IRQs on exit if they were enabled going in. Returns 0. Signature `(src, dst, len)` — distinct from `bim_spi_flash_read`'s `(addr, len, dst)` because the two flash media have separate OEM primitives. Sole caller: `bim_quick_scan_and_launch` (8-byte sniff + 44-byte short-header reads of already-promoted images). |
| named        | `0x00056E0C` | 46  | `bim_iflash_session_begin`   | `flash.c` (extern) | Internal CC2642 flash bring-up. Calls slot [2] of ROM table at `0x100001D8` (= `ROM_API_TABLE` entry 22) with `FLASH_BASE` (`0x40034000`) — likely a "is the FCFG/flash controller ready?" status read. If non-ready, loops calling slot [1] (init) then re-reading slot [2] until ready. Returns the initial readiness state for the matching tear-down. Sole caller: `bim_iflash_program`. |
| named        | `0x0005703C` | 16  | `bim_iflash_program_via_rom` | `flash.c` (extern) | Calls slot [6] of ROM table at `0x100001A8` with `(src, addr, count)` — TI's `FlashProgram` driverlib equivalent — then writes 0 to MMIO `0x42600484` (status clear or VIMS-side gate). Sole caller: `bim_iflash_program`. |
| named        | `0x00057090` | 14  | `bim_iflash_session_end`     | `flash.c` (extern) | Internal flash session tear-down. Takes the bring-up's return value as the "should I tear down?" predicate. If non-zero, calls slot [1] of ROM table at `0x100001D8` — inverse of bring-up. Sole caller: `bim_iflash_program`. |
| named        | `0x0005717C` | 10  | `bim_get_chip_entry`         | `crc.c` (extern)   | Returns the pointer at `g_chip_table_cursor` (`0x20000408`) — after a successful `bim_spi_probe_chip`, this points at the matched 8-byte chip-table entry inside `BIM_CHIP_TABLE_HEAD` (`0x000571A8`). First dword of every entry is the chip's total capacity in bytes (e.g. `0x04000000` = 64 MB for the installed MX25L51245G). Sole caller: `bim_crc32_buffer` (bounds CRC against the actual chip capacity). 6 B + 4 B literal. |
| named        | `0x00057058` | ~24 | `bim_iflash_rom_blank_check` | `flash.c` (extern) | TI ROM-API blank-check primitive — checks whether the 8 KB page at `addr` is entirely `0xFF`. Returns non-zero if blank, 0 if not. Likely dispatches through one of the flash ROM tables at `0x100001A8` / `0x100001D8`. Sole in-source caller: `bim_iflash_check_slot_blank`. |
| named        | `0x00057164` | 6   | `bim_irq_disable_save`       | `flash.c` (extern) | `mrs r0, PRIMASK; cpsid i; bx lr` — disables interrupts, returns prior PRIMASK so the caller can pair-call the matching enable-restore only when it was the one responsible for entering the critical section. Sole caller: `bim_iflash_read`. |
| named        | `0x00057170` | 6   | `bim_irq_enable_restore`     | `flash.c` (extern) | `mrs r0, PRIMASK; cpsie i; bx lr` — mirror of `bim_irq_disable_save`. Sole caller: `bim_iflash_read` (only invoked when the disable saw IRQs enabled). |
| decomp-c     | `0x00056F50` | 32  | `crc32_ieee_byte_step`       | `crc.c`       | 8-iteration CRC32-IEEE per-byte polynomial step (`crc >> 1`, XOR `0xEDB88320` on dropped bit). Equivalent to `crc32_table[byte]` materialised on demand. −10 B vs OEM (GCC's `and+lsrs+cbz+eors` shape vs OEM's carry-flag chain). |
| decomp-c     | `0x00056B1C` | 72  | `bim_slot_iterator`          | `oad.c`       | Walks slots `start_slot..43` (4 KB stride), sniffs each via `FUN_000569E4`, returns slot index on "OAD NVM1" match, `-2` on scan exhaust, `-1` on the unreachable duplicate-match branch. Behaviour-equivalent (−10 B vs OEM). |
| decomp-c     | `0x00056F74` | 32  | `oad_magic_match`            | `oad.c`       | 8-byte equality check against "OAD NVM1" at flash `0x000571E8`. Called by `bim_quick_scan_and_launch` and `bim_slot_iterator`. Same total size (32 B code + 4 B literal) as OEM; GCC walks pointers via post-decrement while OEM uses an indexed counter — behaviour-equivalent. |
| decomp-c     | `0x00056F98` | 32  | `oad_magic_match2`           | `oad.c`       | Byte-identical duplicate of `oad_magic_match`, references the adjacent "OAD NVM1" copy at flash `0x000571E0`. Called only from `bim_slot_iterator`. Duplication is a TI CCS artifact (two translation units each emitted their own static-inlined memcmp + private string copy). Same 32 B + 4 B literal as OEM. |
| decomp-c     | `0x00056254` | 366 | `bim_full_scan_and_launch`  | `oad.c`       | First-boot scan: iterates slots, runs primary CRC + secondary CRC + CRC32 over the image body, promotes a match with `0xFE` markers + launches. Returns 0 normally, -1 on precheck fail. Behaviour-equivalent; `-Os` trims 42 B vs OEM. |
| decomp-c     | `0x00056824` | 130 | `bim_quick_scan_and_launch` | `oad.c`       | Subsequent-boot fast scan: walks slots 0..43, launches the first slot whose status byte is already `0xFE`/`0xFF`. Behaviour-equivalent; `-Os` trims 28 B vs OEM by merging flag and status compare chains. |
| decomp-c     | `0x000568A8` | 120 | `bim_verify_and_launch_image` | `oad.c` | OAD header read → magic check → hash compute (uses `g_hw_id_cached` as salt) → flash-program verified marker → jump-to-entry. Size-equivalent (120 B); stack layout differs. |
| decomp-c     | `0x00057156` | 14  | `bim_launch_image`           | `oad.c`       | Terminal handoff for all three OAD scan paths. Reloads `sp` and `pc` from `*(entry + 4)` (both registers receive the same word — an off-by-one in the OEM's hand-written inline asm that's silent because the launched image's `Reset_Handler` reloads `sp` immediately) and `blx`-es into the launched image. Naked function with inline asm. **Byte-equivalent** to OEM (forced T1 encoding of `adds r0, r0, #4` via `.short 0x1d00`). |
| decomp-c     | `0x00056B64` | 54  | `bim_panic_prep`    | `panic.c`     | 3-step ROM-API handshake + PRCM GPIO clock enable + GPIO_DOE bit-band write to make DIO2 an output. Behaviour-equivalent (+2 B vs OEM). |
| decomp-c     | `0x00056F2A` | 38  | `bim_dispatch`      | `bim.c`       | 3-way dispatcher on image-scan return (`0` → quick scan, `-1` → panic, other → continue). Behaviour-equivalent; GCC trims 4 B vs OEM. |
| decomp-c     | `0x00057000` | 24  | `main`              | `main.c`      | BIM main — caches `(MMIO[0x40032430] & 0xF) << 10` at SRAM `0x20000400`, calls `bim_dispatch`. Behaviour-equivalent only. |
| decomp-c     | `0x00057194` | 8   | `bim_panic_indicate`| `panic.c`     | Single write of `1<<2` to `GPIO_DOUTSET31_0` at `0x40022090` — drives DIO2 (panic LED) high. Behaviour-equivalent (GCC chose `r2/r3` where TI CCS chose `r0/r1`). |
| decomp-c     | `0x000571A0` | 4   | `_system_pre_init`  | `rts_hooks.c` | TI CCS RTS hook, byte-equivalent (`2001 4770`) |
| decomp-c     | `0x000571A4` | 4   | `_exit`             | `rts_hooks.c` | TI CCS RTS hook, byte-equivalent (`bf00 e7fe`) |
| vendor-stock | `0x0005667C` | 108 | `SetupTrimDevice`   | (TI driverlib) | `source/ti/devices/cc13x2_cc26x2/driverlib/setup.c` — device trim called before any MSP/FPU init; busy-waits on a status register at exit. Upstream linked, not vendored. |
| vendor-stock | `0x00056BF0` | 50  | `_auto_init_table`  | (TI CGT RTS)  | TI compiler-runtime cinit descriptor-table walker. Upstream linked, not vendored. |
| named        | `0x00056DD8` | 52  | `ResetISR_body`     | (startup)     | Tail-called from Reset_Handler stub; sets MSP, enables FPU, calls `_system_pre_init` → `_auto_init_table` → `main` → `_exit` |
| named        | `0x00057126` | 10  | `Reset_Handler`     | (startup)     | 10-byte stub: trim + tail-call to body; matches stock TI ResetISR layout |

Status legend:

- **decomp-c** — translated to hand-written C in `src/`.
- **decomp-asm** — preserved as inline assembly because the cycle-
  exact sequence matters (DSB/ISB barriers, atomic compare-swap, etc.)
  and C lowers it differently.
- **vendor-stock** — recognised as a 1:1 copy of an upstream library
  function (TI Driver-lib, CMSIS, newlib). We link against the
  vendored upstream; we don't rewrite it.
- **named** — renamed in Ghidra but not yet translated.
- **pending** — still `FUN_xxxxxxxx`.

(The function table above is the single source of truth — the per-
bucket subsections that used to follow have been folded into it.)

## Upstream linkage

Both vendor-stock entries point at the TI SimpleLink CC13x2/CC26x2
SDK (release 3.40.00.02 brackets the OEM's `Apr 23 2020` build
date). The upstream is **linked from this doc, not vendored into
this repo** at this time; revisit once enough helpers are decoded
that vendoring saves more friction than it costs:

- `SetupTrimDevice` → `source/ti/devices/cc13x2_cc26x2/driverlib/setup.c`
- `_auto_init_table` → TI CGT compiler runtime (`<ccs_install>/tools/compiler/ti-cgt-arm_*/lib/src/_auto_init.c` or similar; not in the SimpleLink SDK itself but ships with the TI ARM Compiler).

Mirrors with browseable trees:
[`jeandudey/simplelink_cc13x2_26x2_sdk`](https://github.com/jeandudey/simplelink_cc13x2_26x2_sdk)
(SDK 3.40 — closest to the OEM build),
[`nanoframework/SimpleLink_CC13xx_26xx_SDK`](https://github.com/nanoframework/SimpleLink_CC13xx_26xx_SDK)
(SDK 5.40 — broader, later),
[`contiki-ng/cc26xxware`](https://github.com/contiki-ng/cc26xxware)
(older CC26x0 driverlib but same `SetupTrimDevice` semantics).

## Open questions

- The MMIO register at `0x40032430` that `main()` reads — sits in
  the `0x40030000..0x40034000` band between the FLASH controller
  and VIMS. Low 4 bits look like a hardware-revision / package
  code, and `bim_verify_and_launch_image` confirms the cached
  `(val<<10)` value is fed into the 376 B hash routine at
  `0x560D8` as a per-device salt. Pin the exact register identity
  once we cross-reference the CC2642R1F TRM.
- ~~`FUN_000560D8` is a hash routine~~ — **resolved**: it's
  `bim_crc32_image`, a plain CRC32-IEEE compute (polynomial
  `0xEDB88320`, init/final XOR `0xFFFFFFFF`). The 2nd argument
  (`g_oad_chunk_size`) is a per-board chunk-size selector, not a
  per-bike salt. The BIM's integrity gate is **not authenticated**
  — anyone who can write the image bytes can write a matching
  CRC32.
- The remaining undecoded helpers around the OAD path:
  `FUN_000570FA` (alt-source read, used by `bim_crc32_image`'s
  and `bim_crc32_buffer`'s alt paths — both dead in this build),
  `FUN_00056D30` (flash-page read, used by `bim_crc32_image`'s
  `use_flash` path).
- ~~`FUN_00056714` is a secondary CRC check~~ — **resolved**:
  it's `bim_iflash_copy_from_spi`, the OAD promote primitive
  that copies a verified candidate from external SPI staging to
  its internal-flash executable destination. The earlier
  "secondary CRC" interpretation was wrong; the function performs
  no CRC compute. The "two CRC checks" in `bim_full_scan_and_launch`
  are actually (1) `bim_crc32_buffer` over the SPI staging copy
  (pre-promote integrity) and (2) `bim_crc32_image` over the
  internal-flash copy (post-promote re-verify).
- ~~`FUN_0005653C` is the primary CRC compute~~ — **resolved**:
  named `bim_crc32_buffer`. Sibling of `bim_crc32_image` with the
  same per-byte algorithm but a flat-buffer call shape.
- ~~`FUN_00056E40` is a small flash read~~ — **resolved**: it's
  `bim_iflash_read`, an IRQ-safe memcpy from memory-mapped
  internal flash. Distinct from `bim_spi_flash_read` (external
  SPI); the quick-scan path uses it because it reads from
  already-promoted images that live in internal flash.
- ~~`FUN_00056CB8` (image-base derivation),
  `FUN_000567A0` (short flash write),
  `FUN_00056E72` (flash program),
  `FUN_00056ED4` (Write Enable)~~ — **resolved**: decoded
  as `bim_oad_find_image_addr`, `bim_spi_flash_program`,
  `bim_iflash_program`, `bim_spi_write_enable`. The three
  internal-flash helpers consumed by `bim_iflash_program`
  (`bim_iflash_session_begin`, `bim_iflash_program_via_rom`,
  `bim_iflash_session_end`) are renamed in Ghidra and
  documented in flash.c as `extern`s; C decomp deferred
  pending exact identification of ROM table at
  `0x100001D8` and MMIO `0x42600484`.
- ~~`FUN_00056CF4` (JEDEC ID read), `FUN_00056AD4` (post-wake
  verifier), `FUN_000569E4` (flash read)~~ — **resolved**:
  decoded as `bim_spi_read_rems_id` (sends REMS opcode `0x90`,
  recv 2 bytes), `bim_spi_wait_wip` (RDSR `0x05` poll loop),
  `bim_spi_flash_read` (READ opcode `0x03` + 24-bit address +
  recv N bytes).
- **Why does the upper 48 MB of the installed 64 MB chip
  appear unreachable from the BIM?** `bim_spi_flash_read`
  uses 3-byte addressing (opcode `0x03`); reaching above
  16 MB would require the 4-byte-address READ4B (`0x13`) or
  the 4-byte-mode-enable opcode (`0xB7`) — neither appears
  in the BIM. Either OAD slots are confined to the first
  16 MB by design, or the upper bank is reserved for
  `bleware`-owned data (BLE bond storage, app NV, log
  buffer). Worth verifying once `bleware` is in scope.
- The OAD image header bytes around file offset `0x1FA8` aren't
  yet parsed; the two `OAD NVM1` markers visible via `strings -t x`
  weren't auto-tagged by Ghidra because they sit inside a packed
  image-header struct that's not 4-byte aligned. A manual
  `DefineString` + struct overlay pass would surface the version /
  length / CRC fields cleanly.

## Decomp policy reminders

The bleboot image is small (≤ 7.9 KB executable), so the function
count is on the order of 30–80 — much smaller than mainware's
~800. Expect a high proportion of `vendor-stock` matches: TI's
Driver-lib `FlashProgram`, `FlashSectorErase`, `ChipInfo_*`, and the
`OAD_evalImageHeader`/`OAD_imgVerify` paths are all stock SDK code.

The custom logic should be confined to:

- the inter-MCU image-transfer wire path (Modbus framing from
  mainware, not TI's reference UART);
- the per-image acceptance policy (VanMoof's signing-key handling);
- the boot-decision state machine that picks among the image
  slot(s).

Translate the customised paths verbatim. Mark the Driver-lib calls
`vendor-stock` and link against TI's published source rather than
rewriting them.
