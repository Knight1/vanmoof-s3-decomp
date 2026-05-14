# shifterboot — decomp progress

Target binary: `shifterboot.bin` (6144 bytes, ARM Cortex-M0, MM32F031F6U6).
Loaded into Ghidra at `0x08000000`. The bootloader sits at the base of
flash and (presumably) jumps to the application at some higher offset
once an integrity check passes.

See `docs/hardware.md` for the canonical binary identity (size, SHA hashes)
and the MindMotion BSP lineage analysis.

## Decomp scope policy

**Decode only VanMoof-custom code.** Functions that are byte-for-byte
copies of canonical vendor sources (MindMotion BSP, MindMotion HAL,
ARM CMSIS) are *recognised* and marked `vendor-stock` — no separate C
translation, no source file in `src/`. The byte-equivalent build will
later pull these in from a vendored copy of the MindMotion BSP, but
the decomp work itself focuses on the bespoke parts of the bootloader.

## Summary

| Count | Status |
| --- | --- |
| ?? | pending (VanMoof-custom, awaiting decomp) |
| 6  | vendor-stock (recognised; no decomp needed) |
| 0  | in-progress |
| 3  | decomp (asm or c) |
| 4  | named (rename in Ghidra, no source yet) |

`function_count = 78` per `ghidra/exports/shifterboot_program.json`.
The pending count will tighten as more functions are classified as
`vendor-stock` vs `custom`.

## Per-module decomp log

- `startup_mm32f031.S` — `Default_Handler` (custom: 2-byte `b .` stub);
  `_cold_reset` + `thunk_main` (VanMoof's 20-byte `__main` replacement
  packed into the unused vector-table tail).
- `systick.c` — `systick_tick` and `SysTick_Handler`. The stock
  MindMotion template has `b .` for SysTick; VanMoof installed a
  countdown decrementer that drives a millisecond-delay variable at
  SRAM `0x20000010`.

## Functions

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |
| `0x0800052e` | 18 | `NVIC_SystemReset` | CMSIS-Core M0 `core_cm0.h` standard inline |
| `0x080005b6` | 8  | `SetSysClock` | MindMotion `system_MM32F031x4x6_q.c` trampoline |
| `0x080005be` | 66 | `SystemInit` | MindMotion `system_MM32F031x4x6_q.c` (byte-identical) |
| `0x0800060c` | 46 | `Reset_Handler` | MindMotion `startup_MM32F031x4x6_q.s` (byte-identical) |
| `0x08001364` | 6  | `USART_SendData` | MindMotion HAL `hal_uart.c` |
| `0x08001372` | 20 | `USART_GetFlagStatus` | MindMotion HAL `hal_uart.c` |
| `0x08001388` | 20 | `USART_GetITStatus` | MindMotion HAL `hal_uart.c` |
| `0x080013ac` | 8  | `CRC_ResetDR` | MindMotion HAL `hal_crc.c` |

The 9 trap stubs at `0x08000632..0x08000644` are byte-identical copies
of `Default_Handler` (one per CMSIS-style exception slot, including
the F1-lineage MemManage/BusFault/UsageFault/DebugMon slots that don't
exist on CM0). They originate from `startup_MM32F031x4x6_q.s`'s
`EXPORT [WEAK]` aliases — treat all 9 as stock; the file contains a
single `Default_Handler` definition.

### VanMoof-custom (decomp targets)

| Address | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x080000b4` | 20 | `_cold_reset` + `thunk_main` | decomp-asm | replaces Keil `__main`, packed into vector-table tail |
| `0x080001d8` | 802 | `main` | named | bootloader's actual logic |
| `0x080014ae` | 20 | `systick_tick` | decomp-c | decrements `g_systick_countdown` if non-zero |
| `0x080014c2` | 8  | `SysTick_Handler` | decomp-c | trampoline to `systick_tick` |
| `0x08001764` | 36 | `_init_data_bss` | named | called from `_cold_reset`; CMSIS-style .data copy + .bss zero — likely VanMoof-written rather than vendor (the MindMotion BSP relies on Keil `__main` instead). To confirm by inspection. |
| `0x0800054c` | 106 | `FUN_0800054c` | pending | likely `SetSysClockToXXM` (called from stock `SetSysClock`); could be either vendor stock or a VanMoof-tuned clock setup — inspect first |
| `0x080000c8` | 28 | `FUN_080000c8` | pending | UART1 single-byte send-and-wait (`USART_SendData` + spin on `USART_GetFlagStatus(TX-done)`). Wrapper is custom; uses HAL primitives. |
| `0x080000e4` | 28 | `FUN_080000e4` | pending | UART1 buffer transmit loop on top of `FUN_080000c8` |
| `0x08000138` | 32 | `FUN_08000138` | pending | iterates a callback over `n` 1-KB pages — likely a flash-erase or CRC-N-pages helper |
| `0x080001bc` | 28 | `FUN_080001bc` | pending | reads a 32-bit value (two halfwords, LE-assembled) from a stream — flash-image header reader |
| `0x08001534` | 32 | `FUN_08001534` | pending | per-page workhorse called from `FUN_08000138`; sends a multi-byte sequence over UART around a page payload |
| `0x08001554` | 34 | `FUN_08001554` | pending | halfword-stream copy: `memcpy_halfwords` via `FUN_080014f8` |
| ... | | | | (~57 more pending) |

Full list in `ghidra/exports/shifterboot_program.json`.

## Open questions

- Where does the bootloader jump to the application image? (vector
  fix-up address / direct branch / `vector_offset` register?)
- What integrity scheme guards the application image? CRC? Hash?
- Does it support OTA updates by itself, or is the update mechanism in
  the application?
- Initial SP `0x200006F8` — what state in SRAM[0x6F8..0x1000) does the
  loader expect to be preserved across the warm jump?
- Is `_init_data_bss` (`0x08001764`) hand-written by VanMoof or borrowed
  from a CMSIS reference startup? (The MindMotion BSP itself relies on
  Keil's `__main` for runtime init; the bespoke `_cold_reset` calls
  this function so it is at least *adapted* to VanMoof's setup.)
- `FUN_0800054c` (called from stock `SetSysClock`) — does it match
  MindMotion's `SetSysClockTo48M` / `SetSysClockTo72M`, or did VanMoof
  customise the clock-tree config?
