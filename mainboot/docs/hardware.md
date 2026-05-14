# mainboot — hardware notes

The main controller bootloader. Runs on the bike's central MCU
(presumably distinct from the eShifter's MM32F031 — far more SRAM,
Thumb-2 instruction set, real Cortex-M3+ system-exception slots).

## Binary identity

| | |
| --- | --- |
| File | `muco-boot.bin` |
| Size | 32768 bytes |
| Version | **unknown** |
| SHA-256 | `16dbd08c90d322b550a5653fd6b15f91e4f67775d17fdcc6f80fed6af53f6043` |
| SHA-512 | `cf8f1e480ed729360a4a83643fb41f6f4e6d085f0ad5faca24eacb7afc0339a6bdcd0657d6a42b9f624e822bea6d86cb3db10faeda6c6e2e0990182c8a309575` |

The hashes are the contract — if a future blob differs by a single byte, it is a different mainboot.

## MCU identification (in-progress)

Confirmed from the OEM image:

- **ARM Cortex-M3 or higher.** Thumb-2 wide encodings present (`ldr.w sp,[pc,#imm]` in `Reset_Handler`), and distinct handlers populate the MemManage / BusFault / UsageFault / DebugMon vector slots (all of which are RESERVED on Cortex-M0/M0+).
- **Soft-float ABI in the toolchain output** (no FPU prologue/epilogue observed in the few inspected functions — assumed; revisit when an FPU-relevant function is decoded).
- **320 KB contiguous SRAM at `0x20000000` – `0x2004FFFF`** — initial SP is `0x20050000`, the byte after the last SRAM address. This narrows the part to:

| Family | Variant | SRAM layout | Match |
| --- | --- | --- | --- |
| STM32F4 | F469/F479 | SRAM1 (160 K) + SRAM2 (32 K) + SRAM3 (128 K) at `0x20000000` = 320 K contiguous | ✅ |
| STM32F7 | F745/F746/F756 | DTCM (64 K) + SRAM1 (240 K) + SRAM2 (16 K) at `0x20000000` = 320 K contiguous | ✅ |
| STM32F4 | F427/F429/F437/F439 | 192 K contiguous | ✗ (top would be 0x20030000) |
| STM32F4 | F405/F407/F415/F417 | 128 K contiguous | ✗ |
| STM32F4 | F446 | 128 K contiguous | ✗ |

The final pick will come from inspecting RCC and peripheral base
addresses in literal pools post-Ghidra. RCC base is `0x40023800` on
both F4 and F7, so it can't disambiguate by itself — USART instance
addresses or FLASH interface offsets will.

## Memory map (provisional)

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (loader)    | `0x08000000` | `0x08007FFF` | mainboot — 32 KB |
| Flash (app, TBC)  | `0x08008000` | … | mainware (assumed) |
| SRAM (loader)     | `0x20000000` | `0x2004FFFF` | mainboot working set, top of stack at `0x20050000` |

The actual application offset is **not yet confirmed** — `0x08008000`
is an STM32 sector-2 boundary on F4xx parts (sector 0 = 16 K, sector
1 = 16 K, total 32 K). To confirm once `main` decodes far enough to
expose the application jump.

## Vector table (head, from raw bytes)

| Slot | Vector | Value | Note |
| --- | --- | --- | --- |
| 0  | initial SP | `0x20050000` | top of contiguous SRAM |
| 1  | Reset      | `0x080041EC` | entry point |
| 2  | NMI        | `0x08003744` | distinct handler |
| 3  | HardFault  | `0x08003746` | distinct handler |
| 4  | MemManage  | `0x08003748` | distinct handler (CM3+ slot, real on this part) |
| 5  | BusFault   | `0x0800374A` | distinct handler |
| 6  | UsageFault | `0x0800374C` | distinct handler |
| 11 | SVCall     | `0x0800374E` | |
| 12 | DebugMon   | `0x08003750` | distinct handler |
| 14 | PendSV     | `0x08003752` | |
| 15 | SysTick    | `0x08003754` | |

The 9 system-exception handlers at `0x08003744..0x08003755` sit on
even 2-byte boundaries (each 2 bytes apart), which is consistent
with a row of `b .` trap stubs — typical CMSIS-template layout. To
verify per-handler once disassembled.

Most IRQ slots (offsets `0x40..0x1D7`) point to `0x0800423C` (the
shared `Default_Handler` — `0xE7FE` = `b .`). A handful of slots are
zero or point at non-default handlers; those non-default slots are
the interrupts mainboot actually services and will be mapped once
the IRQ table is enumerated.

## Reset_Handler synopsis

Lifted from the disassembly at `0x080041EC` (32-byte function):

1. Load `sp` from a literal pool word (`0x20050000`).
2. Copy `.data` from flash `0x08006070` to SRAM `0x20000014..0x200001FC`.
3. Zero `.bss` from SRAM `0x200001FC..0x20000950`.
4. Call `0x080037DC` (presumed `SystemInit` — to confirm).
5. Call `0x08004258` (presumed `main` — to confirm).
6. Fall through to a `bx lr` (should never be reached).
