# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
6 decomp-c / 2 vendor-stock / 2 named / 53 pending
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
| decomp-c     | `0x00057000` | 24  | `main`              | `main.c`      | BIM main — caches `(MMIO[0x40032430] & 0xF) << 10` at SRAM `0x20000400`, calls BIM dispatcher. Behaviour-equivalent only. |
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

## Decoded functions

| Address | Name | Size | Notes |
| --- | --- | --- | --- |
| `0x000568A6` | `HardFault_Handler` | 2 B | `b .` — vendor template's default for the HardFault slot. Surprising that there's no register dump given that mainware's HardFault path does dump faults, but the BIM is tiny and has no UART or RAM-log to dump to. |
| `0x00056C76` | `Default_Handler`   | 2 B | `b .` — catch-all for every unused vector slot (MemManage/BusFault/UsageFault/SVCall/DebugMon/PendSV/SysTick + every IRQ). |
| `0x00056DA2` | `NMI_Handler`       | 2 B | `b .` — distinct symbol so the VT keeps three independent entries, even though all three are byte-identical. |

## Named (in Ghidra) but not yet decompiled

| Address | Name | Size | Notes |
| --- | --- | --- | --- |
| `0x00057126` | `Reset_Handler` | 10 B | Stub: `push {r3,lr}; bl SetupTrimDevice; b.w ResetISR_body`. Bytes at `0x57130..0x57135` are GCC's dead `bl HardFault; pop` epilogue (unreachable). |
| `0x00056DD8` | `ResetISR_body` | 52 B | MSP load + FPU enable + four helper calls (`0x571A0`, `0x56BF0`, `0x57000`, `0x571A4`). Last call is the application `main()`. |

## Vendor-stock functions

| Address | Name | Size | Upstream |
| --- | --- | --- | --- |
| `0x0005667C` | `SetupTrimDevice` | 108 B | `source/ti/devices/cc13x2_cc26x2/driverlib/setup.c` — TI BSD-3 licensed. Called from the Reset_Handler stub *before* MSP/FPU init, which is the canonical TI BIM pattern (the stack pointer at vector-fetch is whatever the boot ROM left in MSP, but it's good enough for one call into trim because driverlib's trim stays under ~32 bytes of stack). Exits with a busy-wait on an unknown status word — likely the AON/MCU domain power-ready bit, also typical of `SetupTrimDevice`. |

Other candidates to check next when picking helpers around the
`ResetISR_body` call graph: the four functions it calls
(`0x571A0`, `0x56BF0`, `0x57000`, `0x571A4`). `0x57000` is 24 B —
the canonical TI `.data` copy + `.bss` zero block lowered to ~24
bytes at `-Os`. `0x571A4` is only 4 B, so it's a jump trampoline
to the real `main` rather than `main` itself.

## Open questions

- Is `Reset_Handler` (62 B) calling a `SystemInit`-style helper, or
  does it inline the clock-tree configuration? At 62 bytes it's most
  likely just `.data` copy + `.bss` zero + `bl main` — the CC2642R1F
  ROM does the clock-tree setup itself.
- The 376-byte `FUN_000560D8` is the largest function in the image
  and lives near the top of `.text`; almost certainly the BIM `main`
  / image-selection routine. Walk into it once a few of the leaf
  helpers around it are recognised.
- One auto-discovered string only (`"BVERApr 23 2020"`). The OAD
  signature words `"OAD NVM1"` show up in the binary but Ghidra
  didn't tag them as strings — likely because they're inside a
  packed image-header struct and not 4-byte aligned. Worth a manual
  `DefineString` pass later.

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
