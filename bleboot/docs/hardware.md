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
| `0x20000300` | 256 | `(crc scratch)` | `crc.c` | 256-byte scratch buffer used by `bim_crc32_image` to stage flash- or RAM-source bytes before the CRC32 inner loop. Adjacent to `g_oad_chunk_size`; the two globals form a contiguous 260-byte block at the start of the BIM's SRAM data. |
| `0x20000400` | 4 | `g_oad_chunk_size` | `main.c` | OAD chunk-size selector — `(MMIO[0x40032430] & 0xF) << 10` cached at boot. Read by `bim_crc32_image` as the outer-loop partition size. Originally misnamed `g_hw_id_cached` (the assumption that it was a per-bike salt turned out to be wrong; it's a per-board configuration selector, not a device identity). |

## Known MMIO accesses (from decomp)

| Address | Width | Direction | Accessor | Notes |
| --- | --- | --- | --- | --- |
| `0x40032430` | 32 b | read | `main` | Sits in the `0x40030000..0x40034000` band between the FLASH controller and VIMS. Low 4 bits select an OAD chunk size (the value is left-shifted by 10 before caching, giving 1024-byte steps from 1 KB to 15 KB). Consumed by `bim_crc32_image` as the outer-loop chunk stride. Probably keyed off a board-revision pad rather than a hardware identity. |
| `0x40022090` | 32 b | write | `bim_panic_indicate`, `bim_flash_prepare`, `dio4_set` | `GPIO_BASE (0x40022000) + DOUTSET31_0 (0x90)` — set-only alias for `DOUT31_0`. Used by three callers with three different DIO bits: `1<<2` (DIO2, panic LED via `bim_panic_indicate`), `1<<3` (DIO3, "flash session active" via `bim_flash_prepare`), `1<<4` (DIO4, "flash op in flight" via `dio4_set` — ~13 call sites across the BIM flash subsystem). |
| `0x400220A0` | 32 b | write | `bim_flash_release`, `dio4_clear` | `GPIO_BASE + DOUTCLR31_0 (0xA0)` — set-only alias for clearing DIO bits. Used as a literal-pool-shared constant across the image: many call sites load `0x400220A0` and either use it directly (`bim_flash_release` clears `1<<4` inline; `dio4_clear` is a standalone leaf doing the same write — ~8 call sites) or `subs r0, #16` to convert it to `DOUTSET31_0` at `0x40022090` (the trick `dio4_set` uses). |
| `0x42441A08` | 32 b | write | `bim_panic_prep` | Bit-band alias of bit 2 of `GPIO_DOE31_0` at `GPIO_BASE + 0xD0 = 0x400220D0`. Writes `1` to switch DIO2 from input (reset default) to output, so the `DOUTSET31_0` write that follows actually drives the pin. |
| `0x40082028` / `0x60082028` | 32 b | write + poll | `bim_panic_prep`, `bim_periph_power_off`, `bim_ssi_init` | `PRCM_BASE (0x40082000) + 0x28` — **PRCM_CLKLOADCTL** (NOT `GPIOCLKGR`, which is at offset `0x48`). Bit 0 = `LOAD` (write 1 to trigger reload of all PRCM clock-gate configuration), bit 1 = `LOAD_DONE` (ack, software-polled). The `0x60082028` alias is the write-through trigger path; the `0x40082028` alias is the readable status. All three PRCM users issue the same trigger-and-wait idiom after any clock-gate change. |
| `0x100001B8` | 32 b | read (ptr-to-table) | `bim_panic_prep`, `bim_periph_power_off`, `bim_ssi_init` | ROM-region dispatch slot — pointer to the TI **PRCM ROM sub-table**. Slot map (consistent across all three callers): `[5]` = PowerDomainOn (`bim_panic_prep(4)`, `bim_ssi_init(6)`), `[6]` = PowerDomainOff (`bim_periph_power_off(6)`), `[7]` = PeripheralRunEnable (`bim_panic_prep(0x500)`, `bim_ssi_init(0x500)` + `(0x100)`), `[8]` = PeripheralReconfigure (`bim_periph_power_off(0x100)` + `(0x500)`), `[13]` = PowerDomainStatus (all three; arg = domain mask, returns `1` = ON in bring-up, `2` = ready-to-power-down in teardown). Exact slot-to-function-name mapping deferred until cross-referenced against TI's `rom.h` from a matching SDK release. |
| `0x100001C4` | 32 b | read (ptr-to-table) | `bim_ssi_init` | ROM-region dispatch slot — pointer to the TI **SSI ROM sub-table**. Slot `[0]` = `SSIConfigSetExpClk`-equivalent (6 args: base, refclk, protocol, mode, bit_rate, data_width); slot `[4]` = FIFO read/drain helper (2 args: base, &out; returns non-zero while data available, 0 when empty). |
| `0x100001B4` | 32 b | read (ptr-to-table) | `bim_flash_prepare`, `bim_ssi_init` | ROM-region dispatch slot — pointer to a TI ROM sub-table that mixes SPI-flash command primitives and IOC/pin-routing. Slot `[15]` called by `bim_flash_prepare` with args `4` then `3` (likely SPI flash "Release from Deep Power Down" `0xAB` + a status follow-up — mirrors `bim_spi_deep_power_down`'s release-side `0xB9`). Slot `[17]` called by `bim_ssi_init` with args `(SSI0_BASE, 6, 5, -1, cfg)` (5 args; likely IOC pin routing for the SSI0 MISO/MOSI/SCLK/CSn lines). |
| `0x40000000` – `0x40000FFF` | 32 b | RMW + via SSI ROM | `bim_ssi_init`, `bim_flash_prepare` | **SSI0** — Synchronous Serial Interface 0, used by the BIM as the SPI master to talk to an external SPI NOR flash chip that stages OAD images. The internal CC2642 flash holds only the BIM itself (this 8 KB page); candidate images live on external SPI flash. This is the TI OAD "external flash" build configuration. Direct register accesses: `+0x04` (CR1, SSE enable), `+0x14` (IM, interrupt mask), `+0x20` (ICR, interrupt clear). |
| `0xE000ED88` | 32 b | read-modify-write | `ResetISR_body` | `SCB->CPACR` — the Reset path sets bits 20–23 (CP10/CP11 full access) to enable the FPU. Standard Cortex-M4F boilerplate. |

## Strings of interest

The image is mostly code — the only ASCII strings are the build
identifier block at the tail and two copies of the OAD NVM marker:

- `BVERApr 23 2020\014:10:12` — build-info block at `0x00057F34`
  (BVER magic + `__DATE__` literal + `__TIME__` literal). The four
  bytes after the time string are the version word `0x00000100`
  ("1.0.0" packed little-endian).
- `OAD NVM1` × 2 — TI OAD image identifier. The copy at flash
  `0x000571E8` is the **comparison string** consumed by
  `oad_magic_match` (`0x00056F74`); call sites are the
  quick-scan slot sniff and the slot iterator. The second copy
  lives in the BIM's own OAD image header near the tail of the
  flash page (the BIM is itself a flashable image, so it carries
  its own header).

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
- ~~Are images verified by CRC only, or by ECDSA signature?~~
  **Resolved**: it's plain CRC32-IEEE (polynomial `0xEDB88320`,
  init/final XOR `0xFFFFFFFF`, first 12 bytes skipped). No
  per-bike binding, no signing, no MAC. The integrity gate is
  bypassable by anyone who can write the image bytes — a forged
  image with a recomputed CRC32 word will pass the BIM's check.
  Authentication, if it exists at all, has to be enforced by a
  higher layer (the OAD reception path that ingests images from
  mainware over the inter-MCU bus, before they reach this CRC
  gate). Decoding `FUN_00056F50` (the per-byte polynomial step)
  is the only remaining piece to fully document the CRC stack;
  the algorithm is universal.
- Which UART/SPI/Modbus path is used to receive an update — TI's
  reference is either USB-CDC (CC2652R8 family) or UART; VanMoof
  almost certainly removed both and routed updates via the on-chip
  BLE OAD profile mediated by mainware.
- Where in the 344 KB app region does `bleware` actually live —
  one slot (single-image BIM) or two (dual-image BIM with A/B
  redundancy)?
