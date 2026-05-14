# shifterboot — decomp progress

Target binary: `shifterboot.bin` (6144 bytes, ARM Cortex-M0, MM32F031F6U6).
Loaded into Ghidra at `0x08000000`. The bootloader sits at the base of
flash and (presumably) jumps to the application at some higher offset
once an integrity check passes.

## Summary

| Count | Status |
| --- | --- |
| 77 | pending |
| 0  | in-progress |
| 1  | decomp-c |
| 0  | named (rename in Ghidra, no C yet) |

`function_count = 78` (after `CreateShifterbootVectorFunctions.java`
materialised the trap-stub leaves at 0x08000632..0x08000644). The image
contains no string literals — pure code.

## Per-module decomp log

- `startup_mm32f031` — `Default_Handler` (`0x08000644`, 2 B). Single
  `b .` infinite-loop trap stub. The Cortex-M0 vector table references
  it from every slot 16..44 except slot 43 (which points at SysTick at
  `0x080014C2`). Eight further 2-byte trap stubs exist at
  `0x08000632..0x08000640` — each pointed at by exactly one of the
  system-exception slots (NMI, HardFault, the reserved CM3-style
  slots, SVC, DebugMon, PendSV) — left pending; expected to be
  byte-identical copies once translated.

## Functions

| Address | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x08000632` | 2 | `FUN_08000632` | pending | trap (NMI slot) |
| `0x08000634` | 2 | `FUN_08000634` | pending | trap (HardFault slot) |
| `0x08000636` | 2 | `FUN_08000636` | pending | trap (reserved CM3) |
| `0x08000638` | 2 | `FUN_08000638` | pending | trap (reserved CM3) |
| `0x0800063a` | 2 | `FUN_0800063a` | pending | trap (reserved CM3) |
| `0x0800063c` | 2 | `FUN_0800063c` | pending | trap (SVC slot) |
| `0x0800063e` | 2 | `FUN_0800063e` | pending | trap (DebugMon slot) |
| `0x08000640` | 2 | `FUN_08000640` | pending | trap (PendSV slot) |
| `0x08000644` | 2 | `Default_Handler` | decomp-c | shared IRQ trap (~30 slots) |
| `0x080014c2` | 8 | `FUN_080014c2` | pending | SysTick — tail-calls `FUN_080014ae` |
| `0x0800060c` | 46 | `Reset_Handler` | pending | privilege check → device-ID-gated config copy → fp call → fp jump |
| `0x080001d8` | 802 | `FUN_080001d8` | pending | largest; called via thunk at `0x080000bc`; likely `main` |

(other 66 functions pending — see
`ghidra/exports/shifterboot_program.json`)

## Open questions

- Where does the bootloader jump to the application image? (vector
  fix-up address / direct branch / `vector_offset` register?)
- What integrity scheme guards the application image? CRC? Hash?
- Does it support OTA updates by itself, or is the update mechanism in
  the application?
- Initial SP `0x200006F8` — what state in SRAM[0x6F8..0x1000) does the
  loader expect to be preserved across the warm jump?
- The reset vector points at `0x0800060C`, but bytes at the tail of
  the vector table (`0x080000B4..0x080000BF`) decode as: load SP from
  pool / `bl 0x08001764` / thunk to `0x080001D8`. This looks like an
  *alternative* reset path. Is `Reset_Handler` at `0x0800060C` the
  warm-reset path while `0x080000B4` is the cold path used after the
  initial SP is loaded by hardware?
