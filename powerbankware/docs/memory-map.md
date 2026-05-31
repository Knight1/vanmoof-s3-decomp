# powerbankware — memory map

Target: **STM32F091xC** (ARM Cortex-M0, 256 KB flash, 32 KB SRAM).
OEM image: `powerbank_firmware_1.11.05.bin` (94 812 B), built Apr 12 2021.

> All facts below are derived from the OEM binary's own header, vector table,
> and reset disassembly — not assumed from batteryware.

## Flash / SRAM

| Region | Start | End | Notes |
| --- | --- | --- | --- |
| `powerbankboot` | `0x08000000` | `0x08008000` | first 32 KB — separate decomp target (own folder) |
| **powerbankware app** | `0x08008000` | `0x0801F25C` | 94 812 B image (header + vectors + code + .data LMA) |
| SRAM | `0x20000000` | `0x20008000` | 32 KB; stack top `_estack = 0x20008000` |

The Cortex-M0 has **no VTOR**: at runtime the app copies its 192-byte vector
table to SRAM `0x20000000` and sets `SYSCFG_CFGR1.MEM_MODE = 3` (SRAM mapped at
`0x00000000`) — done in HAL bring-up, not in the reset handler. (Ghidra image
base for this file is set to `0x08008000`, so Ghidra addr == runtime addr.)

## VanMoof image header (`0x08008000`–`0x08008027`)

| Offset | Bytes | Value | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | `0xAA55AA55` | magic |
| `+0x04` | 4 | `0x011105B2` | version 1.11.05, **type 0xB2 = powerbankware** |
| `+0x08` | 4 | `0x1691DE64` | CRC32 (MPEG-2, post-build patch) |
| `+0x0C` | 4 | `0x0001725C` | imageSize (94 812) |
| `+0x10` | 12 | `"Apr 12 2021\0"` | build date |
| `+0x1C` | 9 | `"10:19:14\0"` | build time |
| `+0x24` | 4 | `0x00010000` | reserved |

## Vector table (`0x08008028`, 48 entries)

| Slot | Value | |
| --- | --- | --- |
| 0 SP | `0x20008000` | top of 32 KB SRAM |
| 1 Reset | `0x08019270` | `Reset_Handler` |
| 2 NMI | `0x080192C0` | = default trap |
| 3 HardFault | `0x0800FE2C` | real handler (TBD) |
| default | `0x080192C0` | `b .` trap (Default_Handler) |

## Reset / .data / .bss (from `Reset_Handler` @ `0x08019270`)

| Symbol | Address |
| --- | --- |
| `_sdata` | `0x200000C0` |
| `_edata` | `0x2000017C` (.data = 0xBC bytes) |
| `_sidata` (LMA) | `0x0801F1A0` (flash, just under image end) |
| `_sbss` | `0x20000180` |
| `_ebss` | `0x20002638` (~9.5 KB .bss) |

Reset: set SP → copy `.data` → zero `.bss` → `SystemInit` (`FUN_0801d938`) →
`__libc_init_array` (`FUN_0801dac4`) → **main** (`FUN_0800f52c`).
