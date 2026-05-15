# bleboot — hardware notes

The BLE-MCU bootloader — the TI **BIM** (Boot Image Manager) for the
BLE radio MCU. Runs once at every reset of the CC2642R1F, picks a
valid application image (`bleware`) out of flash, verifies its OAD
header, and jumps to it. Also services in-the-field OAD (over-the-air
download) updates: receives a new application image via the on-chip
serial bootloader or via the BLE OAD profile (mediated by mainware
over the Modbus inter-MCU bus), writes it to flash, and swaps which
slot is "current".

## Binary identity

| | |
| --- | --- |
| File | `bleboot_1.0.0.bin` |
| Size | 8192 bytes (one CC2642R1F flash sector, 8 KB) |
| Version | `1.0.0` (Apr 23 2020 14:10:12 — `BVER` block at offset `0x1F34`) |
| SHA-256 | `24cdebf92b263b3a1729d1cef6a87de8108217850a574d65a3e1b579fea866af` |

This is the *first* (oldest) shipped bleboot — mirrors
shifterware/mainware's "oldest-as-baseline" policy. A `1.0.1` exists
in the firmware archive and differs by ~3.4 KB; that's a candidate
upgrade target once 1.0.0 is fully reconstructed.

## MCU

**Texas Instruments CC2642R1F** — Cortex-M4F SimpleLink SoC for
Bluetooth Low Energy 5.2. From the TI datasheet (`SWRS213B`):

| | |
| --- | --- |
| Core | ARM Cortex-M4F @ 48 MHz |
| Flash | 352 KB main (`0x00000000..0x0005FFFF`), erase page = 8 KB |
| SRAM | 80 KB at `0x20000000..0x20013FFF` |
| ROM | 256 KB (TI boot ROM + BLE-Stack pieces) at `0x10000000` |
| AUX SRAM | 2 KB sensor-controller scratchpad at `0x400E0000` |
| FPU | FPv4-SP-D16 (single precision) |

CC2642R1F runs **TI's BLE 5.2 SDK** application image (`bleware`) on
top of a multi-layered runtime: the boot ROM at `0x10000000` does the
very-earliest power-on sequencing and consults the CCFG; the BIM in
the last flash page (this image, `bleboot`) selects and verifies an
application; the application contains a copy of TI's BLE-Stack with
SimpleLink Driver-lib calls into the boot ROM.

## Memory map

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (bleware slots) | `0x00000000` | `0x00055FFF` | 344 KB — `bleware` application image(s); BIM picks one to run |
| Flash (bleboot)       | `0x00056000` | `0x00057FFF` |   8 KB — BIM code + OAD-header + CCFG (this file) |
| ROM (TI)              | `0x10000000` | `0x1003FFFF` | 256 KB — TI BLE/SimpleLink boot ROM, Driver-lib helpers |
| SRAM (bleboot)        | `0x20000000` | `0x20013FFF` |  80 KB — initial SP = `0x20014000` (top) |
| Sensor-controller SRAM | `0x400E0000` | `0x400E07FF` |   2 KB — AUX domain (unused by BIM, owned by app) |

The BIM image's 8 KB page contains, in flash order:

```
0x00056000  ┌──────────────────────────────┐
            │ Cortex-M4 vector table       │  initial SP + ≤128 vectors
0x00056xxx  ├──────────────────────────────┤
            │ BIM .text / .rodata          │  code + read-only data
0x00057F30  ├──────────────────────────────┤
            │ BVER block (build info)      │  "BVERApr 23 2020\014:10:12\0" + ver word
0x00057FA8  ├──────────────────────────────┤
            │ OAD image header (88 B?)     │  TI OAD ImageHdr — image ID, length,
            │                              │  type, version, CRC, "OAD NVM1"
0x00057FE0  ├──────────────────────────────┤
            │ CCFG fields (last 32 B)      │  IEEE-BLE, IMAGE_VALID_CONF=0x56000,
            │                              │  bootloader / sector-protection flags
0x00057FFF  └──────────────────────────────┘
```

The CCFG `IMAGE_VALID_CONF` field at `0x00057FEC` contains `0x00056000`
(little-endian `00 60 05 00`), pointing the ROM bootloader back at
this image's own vector table — that's how the ROM finds the BIM at
power-on.

## Vector table (head, from raw bytes — image @ flash `0x00056000`)

| Slot | Vector | Value | Note |
| --- | --- | --- | --- |
|  0 | initial SP | `0x20014000` | top of 80 KB SRAM |
|  1 | Reset      | `0x00057127` | thumb (low bit set) → `0x00057126` |
|  2 | NMI        | `0x00056DA3` | distinct handler |
|  3 | HardFault  | `0x00056867` | distinct handler |
|  4 | MemManage  | `0x00056C77` | → default trap (`b .`) |
|  5 | BusFault   | `0x00056C77` | → default trap |
|  6 | UsageFault | `0x00056C77` | → default trap |
| 7–10 | reserved | `0x00000000` | |
| 11 | SVCall     | `0x00056C77` | → default trap |
| 12 | DebugMon   | `0x00056C77` | → default trap |
| 13 | reserved   | `0x00000000` | |
| 14 | PendSV     | `0x00056C77` | → default trap |
| 15 | SysTick    | `0x00056C77` | → default trap |
| 16+ | IRQ 0+    | `0x00056C77` | all → default trap (≥ 38 slots) |

Most slots route to a shared trap handler at `0x00056C76`
(`b .` — infinite loop). Only NMI/HardFault have distinct entries,
and Reset is the application entry point.

## Known SRAM globals (from decomp)

| Address | Size | Symbol | Module | Notes |
| --- | --- | --- | --- | --- |
| `0x20000400` | 4 | `g_hw_id_cached` | `main.c` | Pre-shifted hardware-revision / package code, cached at boot from MMIO `0x40032430`'s low 4 bits (`(reg & 0xF) << 10`). Read later by the (still-undecoded) BIM dispatcher to pick an application slot. |

## Known MMIO accesses (from decomp)

| Address | Width | Direction | Accessor | Notes |
| --- | --- | --- | --- | --- |
| `0x40032430` | 32 b | read | `main` | Sits in the `0x40030000..0x40034000` band between the FLASH controller and VIMS. Low 4 bits are a hardware-revision / package code; bits 31:4 unused at this call site. Confirmed by `bim_verify_and_launch_image` to be consumed downstream as a per-bike salt fed into the 376 B hash routine at `0x560D8`. Exact register identity not yet pinned to a named CC2642R1F TRM register. |
| `0x40022090` | 32 b | write | `bim_panic_indicate` | `GPIO_BASE (0x40022000) + DOUTSET31_0 (0x90)` — set-only alias for `DOUT31_0`. Writes `1<<2` to drive DIO2 high (panic LED) without disturbing the other 31 pins. |
| `0x42441A08` | 32 b | write | `bim_panic_prep` | Bit-band alias of bit 2 of `GPIO_DOE31_0` at `GPIO_BASE + 0xD0 = 0x400220D0`. Writes `1` to switch DIO2 from input (reset default) to output, so the `DOUTSET31_0` write that follows actually drives the pin. |
| `0x40082028` / `0x60082028` | 32 b | write + poll | `bim_panic_prep` | `PRCM_BASE (0x40082000) + 0x28` — `PRCM_GPIOCLKGR`. The `0x60082028` alias is the write-through path used to set the run-mode GPIO clock gate; the `0x40082028` alias is read to poll bit 1 (the AHB-side `LOAD_DONE`-equivalent ack). After this handshake, GPIO MMIO is safe to touch. |
| `0x100001B8` | 32 b | read (ptr-to-table) | `bim_panic_prep` | ROM-region dispatch slot — holds a pointer to a TI ROM API function table. Indices 5, 7, 13 are called with arguments `4`, `0x500`, `4`. Likely `ROM_API_TABLE[14]` (`ROM_API_UART_TABLE`) in the SimpleLink CC13x2/CC26x2 SDK standard ordering, but the argument values don't fit UART semantics — exact identity left open. |
| `0xE000ED88` | 32 b | read-modify-write | `ResetISR_body` | `SCB->CPACR` — the Reset path sets bits 20–23 (CP10/CP11 full access) to enable the FPU. Standard Cortex-M4F boilerplate. |

## Strings of interest

The image is mostly code — the only ASCII strings are the build
identifier block at the tail and two copies of the OAD NVM marker:

- `BVERApr 23 2020\014:10:12` — build-info block at `0x00057F34`
  (BVER magic + `__DATE__` literal + `__TIME__` literal). The four
  bytes after the time string are the version word `0x00000100`
  ("1.0.0" packed little-endian).
- `OAD NVM1` × 2 — TI OAD image identifier. One copy lives in the
  OAD image header (the BIM's own header, since the BIM is itself
  flashable like an application), and the second copy is likely a
  comparison string for matching uploaded images.

## Provenance

bleboot is a lightly-customised version of **TI's
`bim_offchip` / `bim_onchip` reference BIM** from the SimpleLink
CC13x2/CC26x2 SDK (`source/ti/common/cc26xx/oad/bim/`). The BVER
block and OAD NVM1 marker are TI conventions. We will recognise large
chunks of this code as TI Driver-lib calls into the boot ROM (flash
program/erase, CRC, chip-info reads) and mark them `vendor-stock`,
similarly to how mainware recognises CubeF4 HAL functions.

VanMoof's customisations are likely confined to:

- the per-image acceptance policy (which OAD signing key / version
  range to accept);
- the wire path that ingests new images from mainware over the inter-
  MCU Modbus bus (instead of TI's reference UART/SPI bootloader);
- the version block (`BVER` magic is uniform across all VanMoof
  wares — see shifterware/mainware container format).

## Open questions

- Exact OAD header offset and field layout — `bim_verify_and_launch_image`
  reads the candidate image's header as a 56-byte block (not 88 — that
  matches the older `imgHdr_t` layout from the CC2640R2 SDK rather
  than the newer CC13x2/CC26x2 SDK's 88-byte struct). Confirmed
  fields: `magic` byte (0xFE) at offset 17, `image_addr` u32 at
  offset 24, `entry` u32 at offset 28, `image_hash` u32 at offset 8.
  The remaining 36 bytes are still opaque; walking the BIM's own
  header at `0x00057FA8` against TI's 56-byte `imgHdr_t` should
  surface field names cleanly.
- Are images verified by CRC only, or by ECDSA signature?
  `bim_verify_and_launch_image` compares a single 32-bit hash word —
  too short to be an ECDSA signature, so the gate is either a CRC32
  or a truncated hash. The hash routine is fed `g_hw_id_cached`
  alongside the image, so the result is **per-bike** rather than
  per-image — bricking the image on one bike does not invalidate
  the same image on another bike. Decode the 376 B routine at
  `0x560D8` next to confirm whether it's CRC32-with-salt, a hashed
  MAC, or something custom.
- Which UART/SPI/Modbus path is used to receive an update — TI's
  reference is either USB-CDC (CC2652R8 family) or UART; VanMoof
  almost certainly removed both and routed updates via the on-chip
  BLE OAD profile mediated by mainware.
- Where in the 344 KB app region does `bleware` actually live —
  one slot (single-image BIM) or two (dual-image BIM with A/B
  redundancy)?
