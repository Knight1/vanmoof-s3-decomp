# shifterboot — decomp progress

Target binary: `shifterboot.bin` (6144 bytes, ARM Cortex-M0, MM32F031F6U6).
Loaded into Ghidra at `0x08000000`. The bootloader sits at the base of
flash and (presumably) jumps to the application at some higher offset
once an integrity check passes.

See `docs/hardware.md` for the canonical binary identity (size, SHA hashes).

## Summary

| Count | Status |
| --- | --- |
| 72 | pending |
| 0  | in-progress |
| 2  | decomp-asm |
| 4  | named (rename in Ghidra, no source yet) |

`function_count = 78` per `ghidra/exports/shifterboot_program.json`.

Lineage finding: `Reset_Handler`, the Cortex-M0 vector table, and
`SystemInit` are byte-identical to MindMotion's stock pre-2021 AE-Team
SDK files (`startup_MM32F031x4x6_q.s` + `system_MM32F031x4x6_q.c`).
That means the startup half of the decomp is *recognition* rather
than *reconstruction* — we can transcribe the vendor source line-by-line
once we hit each function. See `docs/hardware.md`.

## Per-module decomp log

- `startup_mm32f031.S`
  - `Default_Handler` (`0x08000644`, 2 B). Single `b .` infinite-loop
    trap stub. The Cortex-M0 vector table references it from every
    IRQ slot in 16..44. Eight further 2-byte trap stubs at
    `0x08000632..0x08000640` are byte-identical copies, one per
    system-exception slot (NMI / HardFault / 3× reserved-on-CM0 /
    SVC / DebugMon / PendSV) — left pending so each is signed-off
    individually.
  - `_cold_reset` + `thunk_main` (`0x080000B4..0x080000C7`, 20 B).
    The OEM's CMSIS-style boot stub: load initial MSP, call
    `_init_data_bss`, pool-thunk into `main`. Reached *via*
    `Reset_Handler`'s tail-`bx`, not directly from the vector table.
    Ghidra fragments this region (first 8 B mis-glued into
    `Reset_Handler`'s body; last 4 B exposed as `thunk_main`); the
    .S decompilation treats it as a single logical stub.

## Functions

| Address | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x080000bc` | 4   | `thunk_main` | decomp-asm | tail of `_cold_reset`; pool-thunk into main |
| `0x080001d8` | 802 | `main` | named | largest function; called via `thunk_main` from `_cold_reset` |
| `0x0800054c` | 106 | `FUN_0800054c` | pending | likely `SetSysClockToXXM` (called from `SetSysClock`); decode once `SetSysClock` is C-translated |
| `0x080005b6` | 8   | `SetSysClock` | named | 8-byte trampoline: `push {r4,lr}; bl FUN_0800054c; pop {r4,pc}`. Matches MindMotion's `SetSysClock → SetSysClockToXXM` template. |
| `0x080005be` | 66  | `SystemInit` | named | byte-identical to MindMotion stock: resets RCC->{CR,CFGR,CIR} to defaults, then tail-calls `SetSysClock`. Confirms `RCC_BASE = 0x40021000` via the data pool at `0x08000600`. |
| `0x0800060c` | 46  | `Reset_Handler` | pending | stock MindMotion: `msr msp,=initial_sp` → if reset vector top byte == 0x1F (entered via on-chip ROM bootloader) enable SYSCFGEN and clear `SYSCFG->CFGR1` → `blx SystemInit` → `bx __main` (which in this build points at `_cold_reset`). Decompile is recognition — transcribe from the canonical vendor source. |
| `0x08000632` | 2   | `FUN_08000632` | pending | trap (NMI slot) |
| `0x08000634` | 2   | `FUN_08000634` | pending | trap (HardFault slot) |
| `0x08000636` | 2   | `FUN_08000636` | pending | trap (reserved CM3 / MemManage slot) |
| `0x08000638` | 2   | `FUN_08000638` | pending | trap (reserved CM3 / BusFault slot) |
| `0x0800063a` | 2   | `FUN_0800063a` | pending | trap (reserved CM3 / UsageFault slot) |
| `0x0800063c` | 2   | `FUN_0800063c` | pending | trap (SVC slot) |
| `0x0800063e` | 2   | `FUN_0800063e` | pending | trap (DebugMon slot) |
| `0x08000640` | 2   | `FUN_08000640` | pending | trap (PendSV slot) |
| `0x08000644` | 2   | `Default_Handler` | decomp-asm | shared IRQ trap (~30 slots) |
| `0x080014c2` | 8   | `FUN_080014c2` | pending | SysTick — tail-calls `FUN_080014ae` |
| `0x08001764` | 36  | `_init_data_bss` | named | called from `_cold_reset`; standard CMSIS .data copy + .bss zero |

(other 64 functions pending — full list in
`ghidra/exports/shifterboot_program.json`)

## Open questions

- Where does the bootloader jump to the application image? (vector
  fix-up address / direct branch / `vector_offset` register?)
- What integrity scheme guards the application image? CRC? Hash?
- Does it support OTA updates by itself, or is the update mechanism in
  the application?
- Initial SP `0x200006F8` — what state in SRAM[0x6F8..0x1000) does the
  loader expect to be preserved across the warm jump?
- The pool-thunk-to-main pattern in `_cold_reset` (vs a 4-byte
  `bl main`) — now explained by the vendor-template lineage: the
  MindMotion `Reset_Handler` ends with `ldr r0,=__main; bx r0`. The
  pool-thunk at `0x080000BC` is the *same pattern* used a second time
  inside the custom `__main` replacement — VanMoof copied the vendor
  idiom rather than introducing a `bl main`.
