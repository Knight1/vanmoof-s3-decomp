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
| SHA-512 | `efcef2f649aca663b6343daf1efda7852bdc59497c1bf3551a0061648ea72686111e4d74bedc92d0d20470131a8d5e83f1a666d2b7daa9f410c75f184dc5d601` |

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
| `0x20000404` | 2 | `g_chip_id_byte1/2` | `flash.c` | REMS readout bytes (manufacturer + device, 2 bytes total) from the external SPI NOR flash chip — populated by `bim_spi_read_rems_id` (sends opcode `0x90`, recv 2 bytes) and consumed by `bim_spi_probe_chip` as the search key into the chip-database table. For the installed Macronix MX25L51245G these read back as `0xC2` (Macronix mfr) and `0x19` (REMS device ID). |
| `0x20000408` | 4 | `g_chip_table_cursor` | `flash.c` | Walk cursor for `bim_spi_probe_chip`'s table search — reset to the table head (`0x000571A8`) at the start of every probe call, advanced by 8 bytes per non-matching entry. The walk-cursor doesn't need to persist across calls, but the OEM keeps it in SRAM (rather than on the stack) — a TI CCS code-size tic. |

## Known MMIO accesses (from decomp)

| Address | Width | Direction | Accessor | Notes |
| --- | --- | --- | --- | --- |
| `0x40032430` | 32 b | read | `main` | Sits in the `0x40030000..0x40034000` band between the FLASH controller and VIMS. Low 4 bits select an OAD chunk size (the value is left-shifted by 10 before caching, giving 1024-byte steps from 1 KB to 15 KB). Consumed by `bim_crc32_image` as the outer-loop chunk stride. Probably keyed off a board-revision pad rather than a hardware identity. |
| `0x40022090` | 32 b | write | `bim_panic_indicate`, `bim_flash_prepare`, `dio4_set` | `GPIO_BASE (0x40022000) + DOUTSET31_0 (0x90)` — set-only alias for `DOUT31_0`. Used by three callers with three different DIO bits: `1<<2` (DIO2, panic LED via `bim_panic_indicate`), `1<<3` (DIO3, "flash session active" indicator LED via `bim_flash_prepare`), `1<<4` (DIO4, **SPI flash /CS release** via `dio4_set` — ~13 call sites bracketing every SPI transaction). |
| `0x400220A0` | 32 b | write | `bim_flash_release`, `dio4_clear` | `GPIO_BASE + DOUTCLR31_0 (0xA0)` — set-only alias for clearing DIO bits. Used as a literal-pool-shared constant across the image: many call sites load `0x400220A0` and either use it directly (`bim_flash_release` clears `1<<4` to drop /CS at session end; `dio4_clear` is a standalone leaf doing the same write to **assert /CS** at the start of every SPI transaction — ~8 call sites) or `subs r0, #16` to convert it to `DOUTSET31_0` at `0x40022090` (the trick `dio4_set` uses). |
| `0x42441A08` | 32 b | write | `bim_panic_prep` | Bit-band alias of bit 2 of `GPIO_DOE31_0` at `GPIO_BASE + 0xD0 = 0x400220D0`. Writes `1` to switch DIO2 from input (reset default) to output, so the `DOUTSET31_0` write that follows actually drives the pin. |
| `0x40082028` / `0x60082028` | 32 b | write + poll | `bim_panic_prep`, `bim_periph_power_off`, `bim_ssi_init` | `PRCM_BASE (0x40082000) + 0x28` — **PRCM_CLKLOADCTL** (NOT `GPIOCLKGR`, which is at offset `0x48`). Bit 0 = `LOAD` (write 1 to trigger reload of all PRCM clock-gate configuration), bit 1 = `LOAD_DONE` (ack, software-polled). The `0x60082028` alias is the write-through trigger path; the `0x40082028` alias is the readable status. All three PRCM users issue the same trigger-and-wait idiom after any clock-gate change. |
| `0x100001B8` | 32 b | read (ptr-to-table) | `bim_panic_prep`, `bim_periph_power_off`, `bim_ssi_init` | ROM-region dispatch slot — pointer to the TI **PRCM ROM sub-table**. Slot map (consistent across all three callers): `[5]` = PowerDomainOn (`bim_panic_prep(4)`, `bim_ssi_init(6)`), `[6]` = PowerDomainOff (`bim_periph_power_off(6)`), `[7]` = PeripheralRunEnable (`bim_panic_prep(0x500)`, `bim_ssi_init(0x500)` + `(0x100)`), `[8]` = PeripheralReconfigure (`bim_periph_power_off(0x100)` + `(0x500)`), `[13]` = PowerDomainStatus (all three; arg = domain mask, returns `1` = ON in bring-up, `2` = ready-to-power-down in teardown). Exact slot-to-function-name mapping deferred until cross-referenced against TI's `rom.h` from a matching SDK release. |
| `0x100001C4` | 32 b | read (ptr-to-table) | `bim_ssi_init` | ROM-region dispatch slot — pointer to the TI **SSI ROM sub-table**. Slot `[0]` = `SSIConfigSetExpClk`-equivalent (6 args: base, refclk, protocol, mode, bit_rate, data_width); slot `[4]` = FIFO read/drain helper (2 args: base, &out; returns non-zero while data available, 0 when empty). |
| `0x100001B4` | 32 b | read (ptr-to-table) | `bim_flash_prepare`, `bim_ssi_init` | ROM-region dispatch slot — pointer to a TI ROM sub-table that mixes SPI-flash command primitives and IOC/pin-routing. Slot `[15]` called by `bim_flash_prepare` with args `4` then `3` (likely SPI flash "Release from Deep Power Down" `0xAB` + a status follow-up — mirrors `bim_spi_deep_power_down`'s release-side `0xB9`). Slot `[17]` called by `bim_ssi_init` with args `(SSI0_BASE, 6, 5, -1, cfg)` (5 args; likely IOC pin routing for the SSI0 MISO/MOSI/SCLK/CSn lines). |
| `0x40000000` – `0x40000FFF` | 32 b | RMW + via SSI ROM | `bim_ssi_init`, `bim_flash_prepare` | **SSI0** — Synchronous Serial Interface 0, used by the BIM as the SPI master to talk to an external SPI NOR flash chip that stages OAD images. The internal CC2642 flash holds only the BIM itself (this 8 KB page); candidate images live on external SPI flash. This is the TI OAD "external flash" build configuration. Direct register accesses: `+0x04` (CR1, SSE enable), `+0x14` (IM, interrupt mask), `+0x20` (ICR, interrupt clear). |
| `0xE000ED88` | 32 b | read-modify-write | `ResetISR_body` | `SCB->CPACR` — the Reset path sets bits 20–23 (CP10/CP11 full access) to enable the FPU. Standard Cortex-M4F boilerplate. |
| `0x50001318` | 32 b | read | `bim_chip_family`, `bim_chip_hw_revision` | `FCFG1_BASE (0x50001000) + 0x318` — `ICEPICK_DEVICE_ID`. Bits [27:12] are PARTNO (`0xBB41` = CC13x2/CC26x2 family identifier); bits [31:28] are the PG (process-generation) revision (silicon stepping). `bim_chip_family` checks PARTNO; `bim_chip_hw_revision` uses the PG nibble to encode the silicon revision. |
| `0x500010A0` | 32 b | read | `bim_chip_hw_revision` | `FCFG1 + 0xA0` — `MINOR_HW_REV` shadow. Low byte = in-PG minor revision (e.g. PG2.1.3); values above `0x7F` are treated as "no minor rev assigned" (returned as 0). |
| `0x5000140C` | 32 b | read | `bim_setup_after_cold_reset_cfg1` | `FCFG1 + 0x40C` — `MISC_TRIM_AON` / `OSC_CONF` block. Bits [17:12] drive the ADI3 byte trim at `0x400CB00E`; bit 9 picks between two channels of the AON trim shadow (offsets `0x13` vs `0x23` from base `0x40086209`); bits [8:6] (mask `0x38`) feed the ADI3 halfword trim at `0x400CB06A`. |
| `0x50004FAC` / `0x50004FB0` / `0x50004FB4` | 32 b | read | `bim_setup_after_cold_reset_cfg1` | Three consecutive FCFG2 trim words (`FCFG2_BASE (0x50004000) + 0xFAC..0xFB4`). FCFG2 isn't documented in the public CC2642R1 TRM; access pattern matches a cold-reset trim shadow that the helper conditionally folds into the AON trim shadow. `0xFB0` bit 1 gates the `0xFAC>>16 \| 0xF0` write to `0x40086256`. `0xFB4` holds the "USER_ID-like" word that the three ROM-HAPI calls each receive as their first arg. |
| `0x400C6000` / `0x400C6004` | 32 b | RMW + poll | `bim_setup_adi_step` | ADI state-machine status (`+0`) and ack (`+4`) — sequencer waits for the two to equal, then writes the next intermediate ADI mode (stepped via an 8-byte LUT at flash `0x000571D8`). Called from `bim_setup_after_cold_reset_cfg1` with target code 2. |
| `0x400CA000` / `0x400CA404` | 16 b / 32 b | dummy-read / write | `bim_setup_after_cold_reset_cfg1` | `DDI_0_OSC_BASE (0x400CA000)` — DDI0 (Direct Digital Interface 0, the RF/clock trim block). Writes `0x01000100` to `+0x404`, then does a 16-bit dummy readback at `+0x00` to push the write through the DDI write-buffer (standard DDI-write idiom). |
| `0x400CB00E` / `0x400CB06A` | 8 b / 16 b | write | `bim_setup_after_cold_reset_cfg1` | `ADI3_BASE (0x400CB000)` — ADI3 (Analog/Digital Interface 3, holds DCDC/recharge/REFSYS trim). Byte write at `+0x0E` and halfword write at `+0x6A` carry the masked bit fields from FCFG1.MISC_TRIM (`0x5000140C`). |
| `0x40086209..0x40086256` | 8 b | write | `bim_setup_after_cold_reset_cfg1` | AON trim shadow byte array (4 byte writes at offsets +0x00, +0x13, +0x23, +0x4D from a misaligned base of `0x40086209` — the misalignment is deliberate, lets the OEM share one literal pool entry across all four writes via `add r1, r4, #imm`). Likely a shadow surfacing into AON_PMCTL's DCDC voltage trim registers. |
| `0x42600494` | 32 b | write (=1) | `bim_setup_after_cold_reset_cfg1` | Bit-banded set of bit 5 in `FLASH+0x24` (= `0x40030024`) — distinct from the bit-1 `BIM_FLASH_ACK_BIT` (`0x42600484`) used by `bim_iflash_program_via_rom` / `bim_iflash_rom_blank_check`. Looks like a "trim-applied" marker bit the BIM signals after every cold-reset cfg1 pass. |
| `0x100001F0` | 32 b | read (ptr-to-table) | `bim_setup_after_cold_reset_cfg1` | ROM-region dispatch slot — pointer to the TI **HAPI / cold-reset ROM sub-table** (= `ROM_API_TABLE[28]`). Three slots called in order from a single load of the sub-table pointer: `[0](r5)`, `[1](fcfg1_rev, r5)`, `[2](r5)` — a per-stepping wakeup state machine that the BIM forwards FCFG2 readouts into. Sub-table identity (HAPI vs another) inferred from the slot count and arg shape; not yet cross-referenced to a specific TI driverlib release. |

## External SPI NOR flash chip (BLE PCB)

The BIM uses SSI0 as the SPI master to talk to an external SPI
NOR flash chip that stages OAD update images.

| | |
| --- | --- |
| Installed chip   | **Macronix MX25L51245GMI-08G-TR** (identified from PCB) |
| Capacity         | 512 Mbit / 64 MB |
| Package          | SOIC-8 (208 mil) |
| Voltage          | 2.7–3.6 V (single supply, matches CC2642 IO domain) |
| REMS ID (`0x90`) | mfr `0xC2` (Macronix), device `0x19` |
| JEDEC ID (`0x9F`)| `C2 20 1A` (mfr `0xC2`, type `0x20` = MX25L, capacity `0x1A` = 512 Mbit) |
| Erase grain      | 4 KB sector / 32 KB block / 64 KB block / chip-erase |
| /CS line         | **DIO4** (CC2642), bit-banged manually rather than driven by SSI0's hardware FSS pin |
| SCK / MISO / MOSI| Driven by SSI0 via IOC — exact DIO assignments TBD (SSI0 IOC config done in `bim_ssi_init` slot [17] call) |

**Bus configuration** (set up in `bim_ssi_init`):

- SPI master, mode 0 (CPOL=0, CPHA=0)
- 4 MHz bit rate (well under the chip's 80 MHz max)
- 8-bit data width
- /CS bit-banged via DIO4 (active low)

**Reachability quirk**: `bim_spi_flash_read` uses 3-byte
addressing (opcode `0x03`, 24-bit address), so the BIM can
only read the first 16 MB of the 64 MB chip. The upper 48 MB
is unreachable from this image — would need the 4-byte-address
READ4B (`0x13`) or a `0xB7` to enable 4-byte mode persistently.
Either OAD slots are confined to the first 16 MB by design, or
the upper bank is reserved for `bleware`-owned data (BLE bond
storage, app NV, log buffer).

### Chip-database table at flash `0x000571A8`

The BIM ships with a hardcoded list of supported SPI NOR flash
chips. `bim_spi_probe_chip` walks this table after every chip
identification, comparing the REMS readout against entries
[4]/[5]. 8-byte entries, NULL-terminated.

| Offset | word[0] (capacity, B) | byte[4] mfr | byte[5] device | byte[6:8] | Identification |
| --- | --- | --- | --- | --- | --- |
| `0x000571A8` | `0x04000000` (64 MB)  | `0xC2` | `0x19` | `00 00` | Macronix MX25L51245G (512 Mbit) — **installed** |
| `0x000571B0` | `0x00200000` (2 MB)   | `0xC2` | `0x15` | `00 00` | Macronix MX25L1606 (16 Mbit) |
| `0x000571B8` | `0x00100000` (1 MB)   | `0xC2` | `0x14` | `00 00` | Macronix MX25L8006 (8 Mbit) |
| `0x000571C0` | `0x00080000` (512 KB) | `0xEF` | `0x12` | `00 00` | Winbond W25X40 (4 Mbit) |
| `0x000571C8` | `0x00040000` (256 KB) | `0xEF` | `0x11` | `00 00` | Winbond W25X20 (2 Mbit) |
| `0x000571D0` | `0x00000000` (term)   | —      | —      | `00 00` | end-of-table sentinel (first word == 0) |

The table presumably reflects VanMoof's BLE PCB design history
across S3 hardware revisions: smaller older chips at the bottom,
the current 64 MB Macronix at the top. The table format
(`{capacity, mfr, dev, padding}`) maps directly to TI's
`extFlashInfo_t` layout from the SimpleLink CC13x2/CC26x2 SDK's
`source/ti/common/cc26xx/flash_interface/external/flash_interface_ext_rtos_NVS.c`.

### ADI step LUT at flash `0x000571D8`

8-byte lookup table consumed by `bim_setup_adi_step`:

| Offset | Bytes |
| --- | --- |
| `0x000571D8` | `01 02 00 03 02 00 01 03` |

Used as a stepping table for the ADI analog-config state machine
at `0x400C6000`: given a current code `c` and target code `t`, the
helper picks `LUT[3 + LUT[c]]` if `LUT[t] <= LUT[c]` (step down)
or `LUT[5 + LUT[c]]` if `LUT[t] > LUT[c]` (step up), so the live
state crosses through a fixed permutation of intermediate codes
rather than jumping directly to the target — analog peripherals
have forbidden direct mode transitions that the LUT routes
around.

### Compiler-runtime auto-init records at flash `0x000571F8`

Handler dispatch table (3 × 4-byte function pointers, indexed by
the type byte of each record):

| Offset | Value | Handler |
| --- | --- | --- |
| `0x000571F8` | `0x00056925` | byte-stream copy (`0x00056924`) |
| `0x000571FC` | `0x00057149` | generic copy (`0x00057148`) |
| `0x00057200` | `0x00057075` | zero-fill (`auto_init_zero_fill` @ `0x00057074`) |

Record table (2 × 8-byte entries, walked by `_auto_init_table` @
`0x00056BF0`):

| Offset | Word 0 (record ptr) | Word 1 (dest) | Effect |
| --- | --- | --- | --- |
| `0x00057218` | `0x00057208` | `0x20000300` | 264-byte zero-fill of CRC scratch + chip-cursor region |
| `0x00057220` | `0x00057210` | `0x20000408` | (record body) — secondary zero-fill |

Records carry their type in byte 0 and a 4-byte size at byte 4
(record body starts at byte 1, so the size is at body+3 — a
misaligned word read the M4 tolerates by default).

### SPI command literals at flash `0x000571F0` / `0x000571F4`

Two pre-formatted SPI command blobs live in the BIM's `.rodata`:

| Address | Bytes | Used by | Notes |
| --- | --- | --- | --- |
| `0x000571F0` | `90 FF FF 00` | `bim_spi_read_rems_id` | REMS opcode `0x90` + 24-bit dummy address `0xFFFF00`. Address LSB = 0 → REMS returns mfr first, device second. |
| `0x000571F4` | `05 06 FF FF` | `bim_spi_wait_wip` (only byte 0) | byte 0 = RDSR opcode `0x05` (Read Status Register). byte 1 = `0x06` (WREN, Write Enable) — present in the literal but **dead** in this build (no caller loads byte 1). May be a leftover from an earlier BIM revision that did flash writes. |

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
