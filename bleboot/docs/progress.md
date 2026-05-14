# bleboot — decompilation progress

Per-function status for the TI BIM (Boot Image Manager) bootloader of
the BLE radio MCU (CC2642R1F). The OEM image is `bleboot_1.0.0.bin`
— 8 KB, loaded at flash `0x00056000`. The image's last 88 bytes are
CCFG (Customer Configuration), so the executable region is a couple
hundred bytes shorter than 8 KB.

## Summary

```
3 decomp-c / 0 vendor-stock / 1 named / 58 pending
```

(`62` total functions discovered by Ghidra auto-analysis at base
`0x00056000`. `4` user-named so far: `Reset_Handler` + the three
exception traps.)

## Per-module log

- **`exception.c`** — Three byte-identical 2-byte `b .` (Thumb
  `0xE7FE`) trap loops at `0x000568A6` / `0x00056C76` / `0x00056DA2`,
  routed from the HardFault, default, and NMI vector slots
  respectively. Compiled back to `e7fe` per handler with the project
  CFLAGS (`-Os -mthumb -mcpu=cortex-m4`); byte-equivalent.

## Function table

| Status | Address | Size (B) | Name | Module | Notes |
| --- | --- | --- | --- | --- | --- |
| decomp-c | `0x000568A6` | 2 | `HardFault_Handler` | `exception.c` | `b .` trap loop |
| decomp-c | `0x00056C76` | 2 | `Default_Handler` | `exception.c` | `b .` trap loop |
| decomp-c | `0x00056DA2` | 2 | `NMI_Handler` | `exception.c` | `b .` trap loop |
| named    | `0x00057126` | 62 | `Reset_Handler` | (startup) | C-runtime + BIM main; not yet decoded |

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
| `0x00057126` | `Reset_Handler` | 62 B | Cortex-M4 startup — copies `.data`, zeroes `.bss`, calls (presumably) `SystemInit` and the BIM `main`. |

## Vendor-stock functions

_None identified yet. Candidates to check first when picking the next
function: TI Driver-lib `FlashSectorErase`, `FlashProgram`,
`HapiSectorErase`, `HapiProgramFlash`, the OAD CRC32 routine (TI's
`crc32_v` variant) — and the SimpleLink ROM trampolines for any of
those._

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
