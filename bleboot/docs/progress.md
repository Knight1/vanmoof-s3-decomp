# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
18 decomp-c / 2 vendor-stock / 2 named / 41 pending
```

(`63` total functions in Ghidra at base `0x00056000`.)

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
- **`flash.c`** — `bim_flash_prepare` at `0x00056A88`. 64 B in
  both OEM and ours (+ 12 B of literal pool in both). Same size,
  behaviour-equivalent. Universal flash-session opener: every BIM
  call that reads or writes flash (`bim_full_scan_and_launch`,
  `bim_verify_and_launch_image`, `bim_crc32_image` in the
  `use_flash` path) calls this first and bails if it returns 0.
  Sequence is: (1) `FUN_000563C8(4_000_000, 9)` — likely a 4 MHz
  reference-clock or timing setup; (2) two ROM-API dispatch calls
  via the table at ROM `0x100001B4` (4 bytes earlier than
  `bim_panic_prep`'s `0x100001B8`, so an adjacent sub-table in
  TI's standard `ROM_API_TABLE` array) with `index 15`, args `4`
  then `3` — likely a wake-from-low-power + arm-sense-amplifier
  pair; (3) light DIO3 (`1<<3` to `GPIO_DOUTSET31_0`) — a
  flash-busy indicator; (4) call `FUN_00057138`, which is known
  to light DIO4 (`1<<4` to the same DOUTSET register) — so the
  combination DIO3+DIO4 is the BLE PCB's "flash session active"
  display; (5) two-stage probe `FUN_00056D6A` then
  `FUN_0005698C`. The OEM's `0x400220A0` literal + `subs r0, #16`
  hints that `0x400220A0` (= `GPIO_DOUTCLR31_0`) is a
  literal-pool-shared constant across many call sites in this
  image; the OEM saves bytes globally by reusing it and adjusting
  per-site. GCC stores the exact `0x40022090` it needs without
  the subtract — different code shape, same byte count overall.
  The complementary "flash session end" routine is `FUN_000570AC`,
  called by every flash-using path (and by this function on the
  failure branch).
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
- **`oad.c`** — Now hosts three sibling functions:
  `bim_verify_and_launch_image` at `0x000568A8`,
  `bim_quick_scan_and_launch` at `0x00056824`, and
  `bim_full_scan_and_launch` at `0x00056254`. All three operate on
  variants of the same OAD-style image header, share the same
  external helper set (CRC compute, hash compute, flash read,
  flash program, slot iterator), and share the same `0xFE`/`0xFC`
  status convention (`0xFE` = verified marker; `0xFC` = rejected;
  `0xFF` = pristine, accepted by the quick path only). The three
  form a tiered boot decision: full → quick → verify-and-launch →
  panic.
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
| decomp-c     | `0x00056A88` | 64  | `bim_flash_prepare`          | `flash.c`     | Universal "flash session begin" precheck. Sets up clock/timing (`FUN_000563C8(4_000_000, 9)`), runs two ROM-API calls through the table at ROM `0x100001B4` (idx 15, args 4 then 3), lights DIO3 + DIO4 as a "flash busy" indicator, then runs a two-stage probe (`FUN_00056D6A` + `FUN_0005698C`). Returns 1 ready, 0 not. Same total size as OEM (64 B + 12 B literal pool). |
| decomp-c     | `0x00056F50` | 32  | `crc32_ieee_byte_step`       | `crc.c`       | 8-iteration CRC32-IEEE per-byte polynomial step (`crc >> 1`, XOR `0xEDB88320` on dropped bit). Equivalent to `crc32_table[byte]` materialised on demand. −10 B vs OEM (GCC's `and+lsrs+cbz+eors` shape vs OEM's carry-flag chain). |
| decomp-c     | `0x00056B1C` | 72  | `bim_slot_iterator`          | `oad.c`       | Walks slots `start_slot..43` (4 KB stride), sniffs each via `FUN_000569E4`, returns slot index on "OAD NVM1" match, `-2` on scan exhaust, `-1` on the unreachable duplicate-match branch. Behaviour-equivalent (−10 B vs OEM). |
| decomp-c     | `0x00056F74` | 32  | `oad_magic_match`            | `oad.c`       | 8-byte equality check against "OAD NVM1" at flash `0x000571E8`. Called by `bim_quick_scan_and_launch` and `bim_slot_iterator`. Same total size (32 B code + 4 B literal) as OEM; GCC walks pointers via post-decrement while OEM uses an indexed counter — behaviour-equivalent. |
| decomp-c     | `0x00056F98` | 32  | `oad_magic_match2`           | `oad.c`       | Byte-identical duplicate of `oad_magic_match`, references the adjacent "OAD NVM1" copy at flash `0x000571E0`. Called only from `bim_slot_iterator`. Duplication is a TI CCS artifact (two translation units each emitted their own static-inlined memcmp + private string copy). Same 32 B + 4 B literal as OEM. |
| decomp-c     | `0x00056254` | 366 | `bim_full_scan_and_launch`  | `oad.c`       | First-boot scan: iterates slots, runs primary CRC + secondary CRC + CRC32 over the image body, promotes a match with `0xFE` markers + launches. Returns 0 normally, -1 on precheck fail. Behaviour-equivalent; `-Os` trims 42 B vs OEM. |
| decomp-c     | `0x00056824` | 130 | `bim_quick_scan_and_launch` | `oad.c`       | Subsequent-boot fast scan: walks slots 0..43, launches the first slot whose status byte is already `0xFE`/`0xFF`. Behaviour-equivalent; `-Os` trims 28 B vs OEM by merging flag and status compare chains. |
| decomp-c     | `0x000568A8` | 120 | `bim_verify_and_launch_image` | `oad.c` | OAD header read → magic check → hash compute (uses `g_hw_id_cached` as salt) → flash-program verified marker → jump-to-entry. Size-equivalent (120 B); stack layout differs. |
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
  read), `FUN_00056E72` (flash program), `FUN_00057156` (image
  launcher), `FUN_000570AC` (post-flash cleanup; the "flash
  session end" complement to `bim_flash_prepare`), `FUN_000570FA`
  (alt-source read), `FUN_00056D30` (flash-page read), and the
  helpers internal to `bim_flash_prepare` itself: `FUN_000563C8`
  (clock/timing setup), `FUN_00056D6A` (first-stage probe),
  `FUN_0005698C` (second-stage probe), `FUN_00057138` (DIO4 set;
  small leaf with a paired DIO4-clear at `FUN_00057188`).
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
