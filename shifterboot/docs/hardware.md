# shifterboot — hardware notes

The bootloader runs on the same MCU as the application
(MM32F031F6U6, Cortex-M0, 32 KB flash, 4 KB SRAM). What follows are
shifterboot-specific observations; the shared eShifter PCB notes live
in the sibling `shifterware/docs/hardware.md`.

## Binary identity

| | |
| --- | --- |
| File | `shifterboot.bin` |
| Size | 6144 bytes (real content `0x17E4` = 6116 B; tail `0x17E4..0x17FF` is `0xFF` flash-erase padding) |
| Version | **unknown** |
| SHA-256 | `4e043e09757b164c4e4785150d2a0bd26bc83ac7fd27d45c7f3f4cce37058aff` |
| SHA-512 | `b08403daf0ec4fec7e80a4e926a3a2e953471bb41aab7e939242ce81b14f556390edad5b2955ea0bbf211dbf5175e2e6eaf59935c200e94801259489167c88bc` |

The hashes are the contract — if a future blob differs by a single byte, it is a different shifterboot.

## Memory map

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (loader) | `0x08000000` | `0x080017FF` | shifterboot — 6 KB |
| Flash (app)    | `0x08001800` | `0x08007FFF` | shifterware (assumed) |
| SRAM (loader)  | `0x20000000` | `0x200006F7` | shifterboot working set |
| SRAM (preserved?) | `0x200006F8` | `0x20000FFF` | ? — initial SP, app may use |

The 6 KB / 26 KB split between loader and application is the working
assumption based on the 6144-byte image size; confirm once the
application's image layout is decoded.

## Provenance: MindMotion BSP fork

The startup code is **not bespoke**. `Reset_Handler` (`0x0800060C`),
the Cortex-M0 vector-table layout, and `SystemInit` (`0x080005BE`) are
byte-identical to MindMotion's stock pre-2021 AE-Team SDK files
`startup_MM32F031x4x6_q.s` and `system_MM32F031x4x6_q.c` (the source
that Keil's `MM32F031_DFP` ships). MindMotion's BSP was itself forked
from ST's `system_stm32f10x.c` — hence the RCC reset masks visible in
`SystemInit` (`0xF0FF0000`, `0xFFF6FFFF`, `0xFFFBFFFF`, `0x009F0000`)
are STM32F1-lineage and not F0-native. Tell-tale F1 holdovers in the
CM0 vector table: the slots for MemManage / BusFault / UsageFault /
DebugMon (CM3+ only, reserved on CM0) are still present and populated
with 2-byte `b .` trap stubs at `0x08000636..0x0800063E`.

Bespoke parts:

- The 20-byte **cold-reset stub** at `0x080000B4` (replacing Keil/ARMCC
  `__main` with a hand-coded SP-load + `_init_data_bss` + pool-thunk
  to `main`, packed into the unused CM0 vector-table tail).
- **`SysTick_Handler` at `0x080014C2`** — the MindMotion stock template
  has just `b .` here; VanMoof installed real logic (tail-calls
  `FUN_080014AE`).
- Everything from `main` (`0x080001D8`) onward.

References:
- <https://github.com/SoCXin/MM32F031/blob/master/src/device/MM32F031x4x6_q/Source/KEIL_StartAsm/startup_MM32F031x4x6_q.s>
- <https://github.com/SoCXin/MM32F031/blob/master/src/device/MM32F031x4x6_q/Source/system_MM32F031x4x6_q.c>
- <https://github.com/ARM-software/CMSIS_4/blob/master/Device/_Template_Flash/Test/system_stm32f10x.c> (ST F10x lineage)
- <https://www.keil.arm.com/packs/mm32f031_dfp-mindmotion/devices/> (Keil pack)

## RCC and SYSCFG bases (confirmed via SystemInit's literal pool)

| Address | Use |
| --- | --- |
| `0x40021000` | `RCC_BASE` — `+0`=CR, `+4`=CFGR, `+8`=CIR |
| `0x40021018` | `RCC->APB2ENR` (touched by Reset_Handler's bootloader-recovery branch to enable SYSCFGEN) |
| `0x40022000` | `FLASH_BASE` (present in SystemInit's data pool at `0x08000604`, not yet observed in use by `SystemInit` itself; likely used by `SetSysClock` for ACR latency) |
| `0x40010000` | `SYSCFG_BASE` — `+0`=CFGR1 (cleared by Reset_Handler when entered via on-chip ROM bootloader) |

## GPIO pinout (USART1 — Modbus RTU)

Confirmed by `boot_init_usart1` (`0x08001578`):

| Pin | Function | GPIO config | Notes |
| --- | --- | --- | --- |
| **PB6** | USART1_TX | `AF0`, push-pull, 50 MHz | data out to the bike's internal Modbus bus |
| **PB7** | USART1_RX | `AF0`, floating input | data in from the bus; drives `USART1_IRQHandler` |

The shifter is a Modbus slave on the bus shared with the main module
(master) and the other sub-modules. The TX/RX pin choice (PB6/PB7) is
the canonical MM32F031 USART1 pin pair on the LQFP-20 package; a
half-duplex driver or TX-enable line (if any) would live elsewhere on
the PCB and isn't visible from the firmware-level decomp yet.

## Inter-module bus parameters

| Parameter | Value | Source |
| --- | --- | --- |
| Protocol | Modbus RTU (slave) | function-code dispatch + 0xA001 CRC |
| Baud rate | **9600** | `main` calls `boot_init_usart1(75 << 7)` at `0x08000214` |
| Word length | 8 data bits | `USART_InitTypeDef.USART_WordLength = 0x30` |
| Stop bits | 1 | `USART_StopBits = 0` |
| Parity | none | `USART_Parity = 0` |
| RX buffer | SRAM `0x200000C4`, 45 bytes | overflow drops silently |
| RX index | SRAM `0x20000014`, halfword | reset by the dispatcher after consuming a frame |

The 45 B RX ceiling matches the largest PDU shifterboot expects to
service (cmd-0x82 OTA stream: 11 B header + 32 B payload + 2 B CRC).

## SysTick

`boot_init_systick` (`0x08001472`) configures SysTick for a **1 ms
tick at HCLK = 48 MHz** (reload value `47999` = `48000 - 1`,
`CLKSOURCE | TICKINT | ENABLE`), then raises the SysTick exception
priority from CM0-lowest (3) to highest (0). Drives the millisecond
countdown at SRAM `0x20000010` consumed by `mdelay`.

The 48 MHz figure is now corroborated by three independent pieces of
evidence: `set_sysclock_to_48m`'s clock-tree config, the mainware-side
`rcc_get_clocks_freq` interpretation, and SysTick's `48000` reload.

## Vector table (head, from raw bytes)

| CM0 idx | Vector | Value | Note |
| --- | --- | --- | --- |
| 0 | initial SP | `0x200006F8` | upper 1.79 KB of SRAM reserved |
| 1 | Reset      | `0x0800060C` | entry point |
| 2 | NMI        | `0x08000632` | (likely trap) |
| 3 | HardFault  | `0x08000634` | (likely trap) |
| 11 | SVCall    | `0x0800063C` | |
| 14 | PendSV    | `0x08000640` | |
| 15 | SysTick   | `0x080014C2` | |

Other vector slots populate the 0x08000632–0x08000640 cluster, which
suggests a row of `for(;;)` trap stubs — same pattern as in shifterware.
