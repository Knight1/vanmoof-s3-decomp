# shifterboot — hardware notes

The bootloader runs on the same MCU as the application
(MM32F031F6U6, Cortex-M0, 32 KB flash, 4 KB SRAM). What follows are
shifterboot-specific observations; the shared eShifter PCB notes live
in the sibling `shifterware/docs/hardware.md`.

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
