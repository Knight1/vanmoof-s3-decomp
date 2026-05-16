# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
29 decomp-c / 2 vendor-stock / 2 named / 30 pending
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
- **`flash.c`** — Now hosts 11 functions covering the full
  external SPI flash session: session begin/end
  (`bim_flash_prepare`, `bim_flash_release`), PRCM teardown
  (`bim_periph_power_off`), SSI0 bring-up (`bim_ssi_init`),
  DPD bracket (`bim_spi_deep_power_down`, `bim_spi_release_from_dpd`),
  chip identification (`bim_spi_probe_chip`), SPI primitive
  (`bim_spi_send_bytes`), DPD-landed verification
  (`bim_spi_wait_idle`), and the two DIO4 indicator leaves
  (`dio4_set`, `dio4_clear`). The DIO4 pair is widely shared
  across the flash subsystem: `dio4_set` has 13 call sites,
  `dio4_clear` has 8.
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
  `FUN_00056CF4` (JEDEC ID read, still pending; presumably
  sends `0x9F = Read ID` and stashes the response at SRAM
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
  via `bim_spi_send_bytes`, bracketed with DIO4 indicator
  toggle; (2) tRES1 wake-up delay via a 200-iter `uint16_t`
  spin loop (~12 µs at 48 MHz HF — within JEDEC's 3–30 µs
  spec window); (3) verify the chip via `FUN_00056AD4` (still
  pending). Returns 1 on success. Sole caller:
  `bim_flash_prepare` as the gate between SSI bring-up and
  the chip-probe tail. The "send failed → fail" branch is
  dead in this build (`bim_spi_send_bytes` always returns 0)
  but preserved verbatim. The C reconstruction uses a
  `volatile uint16_t` counter for the delay; GCC will emit
  a different loop shape than the OEM's
  `mov r0, r1; subs r1, r0, #1; cmp r0, #0; bne` pattern
  but the iteration count and total delay are preserved.
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
- **`crc.c`** — Now hosts two functions: `bim_crc32_image` at
  `0x000560D8` (the outer CRC compute) and `crc32_ieee_byte_step`
  at `0x00056F50` (the per-byte polynomial step). Together they
  implement standard CRC32-IEEE (polynomial `0xEDB88320`,
  init/final XOR `0xFFFFFFFF`) without a precomputed table — the
  step function materialises one table entry on demand by 8-bit
  polynomial division. Trades ~8 cycles per byte for ~1 KB of
  flash savings, which fits the BIM's 8 KB budget.
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
  `FUN_000569e4(0, 56, &hdr)`, bails if `hdr.status != 0xFE`,
  computes an image-base word via `FUN_00056cb8(0, hdr.image_size)`,
  runs `FUN_00056714(0, hdr.image_size, base)` as a secondary
  geometry check, then computes the CRC32 over the image body
  via `bim_crc32_image(page, g_oad_chunk_size, 0, hdr.image_size, 0)`.
  On match against `hdr.image_crc`, writes a 1-byte `0xFE` marker
  into flash at offset 17 of the image's own page via
  `FUN_00056e72` and jumps to `hdr.entry` via `FUN_00057156`. The
  `0xFC` write to the local `status` byte preceding the compare
  is dead on the mismatch path but materialised by `-Os` because
  the C source has the default value inline with the declaration.
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
  header read (`r4<<12`) and **8 KB stride** for the verified-marker
  write (`r4<<13`) — the BIM keeps a tighter grid for metadata
  than for image bodies. For each slot it: (a) primary CRC check
  via `FUN_0005653c` against `hdr[8]`; (b) on match, reads a
  44-byte secondary header from a fixed `slot_base = 0` anchor (a
  single global metadata buffer, not the slot itself); (c) derives
  the image base via `FUN_00056cb8`, runs secondary CRC via
  `FUN_00056714`, writes a transient `0xFC` to `slot+16`; (d)
  computes CRC32 over the image body via
  `bim_crc32_image(page, g_oad_chunk_size, slot_base, derived_len,
  0)` and compares against `hdr2[8]`; (e) **promotes** by writing
  `0xFE` to both the slot's `+17` byte (in metadata) and the
  image-page's `+17` byte (in flash), then launches via
  `FUN_00057156(hdr2[28])` if the flags byte at offset 18 is in
  `{0, 1, 3, 7}`. Mismatches write `0xFC` to `slot+17` (reject)
  and clear the transient `+16` marker. Returns 0 after the
  iterator exhausts all slots (or after a successful launch
  returns, which shouldn't happen in practice but the compiler
  doesn't know that). Note: the `slot_base = 0` register is held
  constant by the OEM in r5 across the entire function but never
  modified — so several flash writes that look like they take a
  slot-relative address (`slot_base + 17`, `slot_base + 16`) hit
  literal flash addresses 16/17 in this build. Preserved as-is;
  the underlying `FUN_000567a0` likely interprets a 0 anchor as
  "use a different implicit base via global state."
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
| decomp-c     | `0x00056D6A` | 56  | `bim_spi_release_from_dpd`   | `flash.c`     | Sends JEDEC `0xAB` (Release from DPD), waits ~12 µs (200-iter spin loop for tRES1), then verifies via `FUN_00056AD4` (pending). Returns 1 on success. Sole caller: `bim_flash_prepare` as the bring-up gate before chip-probe. |
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
  `FUN_000569E4` (flash read), `FUN_00056CB8` (image-base
  derivation), `FUN_00056714` (secondary CRC check),
  `FUN_000567A0` (short flash write), `FUN_00056E40` (small flash
  read), `FUN_00056E72` (flash program), `FUN_000570FA`
  (alt-source read), `FUN_00056D30`
  (flash-page read), the helpers internal to `bim_flash_prepare`
  itself: `FUN_00056CF4` (JEDEC-ID read helper consumed by
  `bim_spi_probe_chip`), `FUN_00056AD4` (post-wake verifier
  consumed by `bim_spi_release_from_dpd`).
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
