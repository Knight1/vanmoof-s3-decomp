# shifterboot — decomp progress

Target binary: `shifterboot.bin` (6144 bytes, ARM Cortex-M0, MM32F031F6U6).
Loaded into Ghidra at `0x08000000`. The bootloader sits at the base of
flash and (presumably) jumps to the application at some higher offset
once an integrity check passes.

See `docs/hardware.md` for the canonical binary identity (size, SHA hashes).

## Summary

| Count | Status |
| --- | --- |
| 74 | pending |
| 0  | in-progress |
| 2  | decomp-asm |
| 2  | named (rename in Ghidra, no source yet) |

`function_count = 78` per `ghidra/exports/shifterboot_program.json`.

(no `decomp-c` entries yet — startup is asm-source by nature)

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
| `0x0800060c` | 46  | `Reset_Handler` | pending | privilege check → device-ID-gated SYSCFG remap → call `FUN_080005BE` → tail-jump to `_cold_reset` |
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
- Reset_Handler's data pool at `0x08000648..0x08000668` references
  `RCC->APB2ENR` (`0x40021018`) and `SYSCFG->CFGR1` (`0x40010000`)
  and only acts on them if the reset-vector slot's top byte equals
  `0x1F` (System Memory). That's a re-entry path from the on-chip
  UART/USB bootloader. Confirms the loader is intended to coexist
  with the MM32 factory ROM.
- The OEM uses an explicit pool-thunk (`ldr r0, =main; bx r0`) instead
  of `bl main`. Why? Likely hand-written asm where the author wanted
  no `lr` link-back into the stub, but `bl` would work just as well —
  no clear functional reason. May be a code-style artifact.
