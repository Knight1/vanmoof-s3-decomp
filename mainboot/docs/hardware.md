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

## Provenance: Muco Technologies third-party bootloader

This is **not VanMoof-written**. The image contains the banner
string:

```
'MT' (@) 2019 STM32F4, Start
STM32 bootloader <%X.%02X> Muco Technologies (c)2019
```

It is a commercial STM32F4 bootloader product from **Muco
Technologies** (a Dutch firm; the same vendor identifier seen in
the `Muco-` filename prefix). VanMoof appears to have licensed this
loader and integrated it as the main-controller first-stage; the
"customisation" likely comes from build-time configuration (image
table layout, peripheral assignments, CRC seed) rather than source
edits.

Consequence for the decomp scope policy: nearly the entire
function set is third-party code, not bespoke. We still translate
it (clean-room reconstruction, original behaviour described in
sources we author), but expectations about which functions are
"interesting" are inverted — *every* function is Muco IP, and the
bespoke layer is the application-level data that Muco's loader
consumes (image table format, slot layout, version word
encoding).

## MCU

**ST STM32F413VGT6** (confirmed by Tobias). Cortex-M4F, 100-pin
LQFP, 1 MB flash, 320 KB SRAM (256 K SRAM1 at `0x20000000` + 64 K
SRAM2 at `0x20040000`, contiguous → top at `0x20050000`, matching
the OEM initial SP).

The mainware application runs on the same MCU; the loader uses
only the first 32 KB of flash and the bottom of SRAM.

In-binary corroboration:
- Banner strings `"'MT' (@) 2019 STM32F4, ..."` and `"No F4
  code\r\n"`.
- Thumb-2 wide encodings (`ldr.w sp,[pc,#imm]` in `Reset_Handler`).
- Distinct handlers populate the MemManage / BusFault / UsageFault /
  DebugMon vector slots — all RESERVED on Cortex-M0/M0+, so this
  must be M3+.
- The vector table has a non-default handler at slot 98, consistent
  with the F413's IRQ count (~97 used IRQs in the F413 SVD).

Build flags: `-mcpu=cortex-m4`. Float ABI is `soft` for now — the
F413 does have a single-precision FPU (FPv4-SP-D16) and we may
need to switch to `-mfpu=fpv4-sp-d16 -mfloat-abi=hard` once a
function with FPU prologue/epilogue surfaces.

## Memory map (provisional)

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (loader)    | `0x08000000` | `0x08007FFF` | mainboot — 32 KB (= sectors 0+1 on F4) |
| Flash (app)       | `0x08008000` | … | "Loaded Application" + per-subsystem firmware blobs |
| Flash (shadow)    | … (TBC)      | … | "Shadow Application" staging slot for verified copies |
| SRAM (loader)     | `0x20000000` | `0x2004FFFF` | mainboot working set, top of stack at `0x20050000` |

The mainboot doesn't host one application — it hosts **all** the
S3 subsystems' firmware blobs. The strings expose at minimum:

- *Loaded Application* (active, executable)
- *Shadow Application* (staging copy that is CRC-verified, then
  copied over the loaded slot — A/B updates with verify+commit)
- *Shifter Application*, *Motor Application*, *Battery Application*
  (per-subsystem firmware payloads, presumably delivered to the
  eShifter / motor / battery MCUs over Modbus once mainboot
  hands control to mainware)

The exact slot offsets are TBC from decoding the image-table
walker — the `"Erase sector %d"` and `"Erasing shadow flash..."`
strings show that flash-sector erase happens here.

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
