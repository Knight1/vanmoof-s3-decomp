# mainboot — hardware notes

The main controller bootloader. Runs on the bike's central MCU
(presumably distinct from the eShifter's MM32F031 — far more SRAM,
Thumb-2 instruction set, real Cortex-M3+ system-exception slots).

## Binary identity

| | |
| --- | --- |
| File | `muco-boot.bin` |
| Size | 32768 bytes |
| Version | **v1.09**, built Feb 21 2020 14:50:53 (see Version footer below) |
| SHA-256 | `16dbd08c90d322b550a5653fd6b15f91e4f67775d17fdcc6f80fed6af53f6043` |
| SHA-512 | `cf8f1e480ed729360a4a83643fb41f6f4e6d085f0ad5faca24eacb7afc0339a6bdcd0657d6a42b9f624e822bea6d86cb3db10faeda6c6e2e0990182c8a309575` |

The hashes are the contract — if a future blob differs by a single byte, it is a different mainboot.

### Version footer

The loader carries a 40-byte `vanmoof_ware_t`-style record in the **last
`0x28` bytes** of the image (at `0x08007FD8`) — same struct as the wares,
but at the tail rather than the head:

| Off | Field | Value in this blob |
| --- | --- | --- |
| `0x08007FD8` | magic | `0xAA55AA55` |
| `0x08007FDC` | version[4] | `ff ff 09 01` → major `version[3]`=1, minor `version[2]`=9 |
| `0x08007FE0` | crc | `0xFFFFFFFF` (**unset**) |
| `0x08007FE4` | length | `0xFFFFFFFF` (**unset**) |
| `0x08007FE8` | date[12] | `"Feb 21 2020"` |
| `0x08007FF4` | time[12] | `"14:50:53"` |

So the loader is **v1.09**. The banner at `0x080058BC`
(`"STM32 bootloader v%x.%02x (%s %s)"`) reads major/minor from
`version[3]`/`version[2]` and the two `%s` from the date/time fields.

The loader's **own crc/length are left blank** — mainboot is *not*
self-CRC-verified. The `*CRC*` strings ("APP CRC error", "Shadow CRC
error", …) refer to the **application images** it loads, each of which has
its own `vanmoof_ware_t` header (magic `0xAA55AA55` + crc + length) at its
flash base. The version/info command's literal pool at `0x08002A80`
enumerates those slots:

| Image | Base | "missing" string |
| --- | --- | --- |
| Loaded Application | `0x08020000` | "No Application" |
| Shadow Application | `0x08060000` | "No Shadow Application" |
| Shifter Application | `0x08010000` | "No Shifter Application" |
| Motor Application | `0x080A0000` | "No Motor Application" |
| Battery Application | `0x080C0000` | "No Battery Application" |

Each is printed via `"… Application: v%x.%02x.%02X (%s %s)"` — i.e. app
versions are major.minor.**patch** (`version[3].[2].[1]`), one more field
than the loader's own major.minor banner.

## Provenance: Muco Technologies third-party bootloader

This is **not VanMoof-written**. The image contains the banner
string:

```
'MT' (@) 2019 STM32F4, Start
STM32 bootloader <%X.%02X> Muco Technologies (c)2019
```

**Muco Technologies B.V.** is a Dutch embedded-engineering
contractor (part of the MACH Technology Group, alongside Head
Electronics B.V., CDS Electronics, and Venne). VanMoof appears to
have outsourced the main-controller first-stage to them; the
"customisation" likely comes from build-time configuration (image
table layout, peripheral assignments, CRC seed) rather than source
edits. The multi-image layout (Loaded / Shadow / per-subsystem
firmware blobs) and the "Copy Shadow to App" + "Jump" flow are
**Muco-specific** — no upstream ST or open-source bootloader has
this structure.

### Upstream ST code expected inside this image

Muco's bootloader sits on top of ST's STM32Cube ecosystem. We
expect to find — and will mark `vendor-stock` when identified —
the following upstream sources:

- **STM32CubeF4 HAL/LL** (`STM32Cube_FW_F4`): `HAL_FLASH_*`
  (used by the "Erase sector %d" / "Erasing shadow flash..." flow),
  `HAL_CRC_*` (used by the "Shadow CRC error" / "APP CRC error"
  integrity checks), `HAL_UART_*` (used by the printf banner
  output), `HAL_RCC_*` for clock setup, `HAL_GPIO_*`, possibly
  `HAL_TIM_*` if timing is required.
- **CMSIS-Core** intrinsics (`__DSB`, `__ISB`, `NVIC_SystemReset`,
  `NVIC_SetPriority`, `__disable_irq`, `__enable_irq`, etc.).
- **CMSIS-Device** startup file (vector table layout, `SystemInit`,
  `Reset_Handler` — these may or may not be byte-identical;
  Muco's reset path is custom enough to be recognisable on its
  own).

What is **not** in scope as upstream (verified by web search):

- ST's `stm32-mw-openbl` ("Open Bootloader"): uses different
  protocols (the on-chip ROM bootloader's USART/I2C/SPI/USB-DFU
  AN3155 family). No string or protocol overlap with muco-boot.
- ST's CubeF4 IAP example (`STM32Cube_FW_F4/Projects/<board>/
  Applications/IAP/`): structurally simpler (single
  application slot, YMODEM upload). Muco's multi-image scheme
  is its own design.

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
