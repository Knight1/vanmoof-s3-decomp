# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
12 decomp-c / 2 vendor-stock / 2 named / 47 pending
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
  `FUN_000569e4(0, 56, &hdr)`, bails if `hdr[17] != 0xFE`, computes
  an image-base word via `FUN_00056cb8(0, hdr[24])`, runs
  `FUN_00056714(0, hdr[24], base)` as a secondary geometry check,
  then feeds `(base>>13)&0xff` (page number), `g_hw_id_cached`,
  and `hdr[24]` into the 376 B hash routine at `0x560D8`. On hash
  match it writes a 1-byte `0xFE` marker into flash at offset 17
  of the image's own page via `FUN_00056e72` and jumps to
  `hdr[28]` (entry word) via `FUN_00057156`. The `0xFC` write to
  the local `status` byte preceding the compare is dead on the
  mismatch path but materialised by `-Os` because the C source
  has the default value inline with the declaration.
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
  hashes the image via `FUN_000560d8(page, g_hw_id_cached, 0, 0)`
  and compares against `hdr2[8]`; (e) **promotes** by writing
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
| decomp-c     | `0x00056254` | 366 | `bim_full_scan_and_launch`  | `oad.c`       | First-boot scan: iterates slots, runs primary CRC + secondary CRC + hash with hw-id salt, promotes a match with `0xFE` markers + launches. Returns 0 normally, -1 on precheck fail. Behaviour-equivalent; `-Os` trims 42 B vs OEM. |
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
- `FUN_000560D8` (376 B) is the largest function in the image and
  is the **hash routine** called from both `bim_verify_and_launch_image`
  and `bim_full_scan_and_launch`. Fed with the page number,
  `g_hw_id_cached` as a per-bike salt, and the image body anchor.
  Returns a 32-bit value compared against the header's
  `image_hash` field. Likely CRC32-with-salt or a truncated hash
  MAC (too short to be ECDSA); decoding it should determine
  whether the BIM's per-bike trust gate is reversible.
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
