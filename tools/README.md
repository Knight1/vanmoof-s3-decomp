# Cross-firmware tools

## `patch_image_header.py`

Finalises a VanMoof firmware's checksum in a built `.bin`. Each ware's
`Makefile` calls it as the post-`objcopy` step
(`python3 ../tools/patch_image_header.py $@`). It auto-detects the format from
the leading magic — the same dispatch as chwdt/vanmoof-tools `crc32.c` — and
handles both firmware layouts:

**1. Application ware** (magic `0xAA55AA55` at `+0x00`) — batteryware,
shifterware, mainware, … . The 40-byte front header carries two fields only
known after the final size:

| Offset | Field | Action |
| --- | --- | --- |
| `+0x08` | `crc32` | computed and written |
| `+0x0C` | `imageSize` | set to the file length (authoritative) |

CRC is over the whole image with `crc`+`imageSize` (header bytes `[8:16)`)
blanked to `0xFFFFFFFF` first.

**2. Bootloader binary** (no `0xAA55AA55` magic; raw vector table first) —
shifterboot, mainboot, bmsboot. The checksum lives in an 8-byte **trailer**:

| Offset | Field | Action |
| --- | --- | --- |
| `[len-8 : len-4]` | `version` | left untouched (build-set) |
| `[len-4 : len]` | `crc32` | computed and written |

CRC is over `image[0 : len-4]` (everything except the trailing CRC slot) —
crc32.c's "assume boot-loader binary" path.

**3. BLE ware** (magic `"OAD NVM1"` at `+0x00`) — bleware, on the CC2642. TI
OAD-NVM1 header:

| Offset | Field | Action |
| --- | --- | --- |
| `+0x08` | `crc32` | computed and written |
| `+0x18` | `len` | image length — trusted from the header, validated `<= file size` |

CRC is over `image[12 : len]` (after the `imgID`+`crc` fields, through `len`).
It covers the `crcStat` byte (`+0x11`), which must be `0xFF` at build time — its
state in a freshly-built, un-promoted image — so the result matches the OEM
precomputed CRC. crc32.c's BLE branch: `crc32(0, data + 12, len - 12)`.

**CRC algorithms**: formats 1 & 2 use **MPEG-2** CRC32 (poly `0x04C11DB7`, init
`0xFFFFFFFF`, no reflection, over little-endian 32-bit words) — what the
STM32/MM32 hardware CRC peripheral computes. Format 3 uses the **standard zlib
CRC-32** (poly `0xEDB88320` reflected, init+xorout `0xFFFFFFFF`) — a *different*
checksum. Verified against the OEM batteryware (1.17.1, 1.14.1) and shifterware
(0.237) images and against crc32.c's `crc32_calculate` / `ware_crc` / BLE branch.

**motorware** (C2000) has no dedicated handling — it carries neither magic and
would fall into the bootloader path, which is not known to be correct for it.

```
python3 tools/patch_image_header.py <image.bin> [<image2.bin> ...]
```

Accepts multiple images (patched independently); exit 0 if all succeed, 1 if any
fail. This supersedes the former per-ware copies (`batteryware/tools/
patch_image_header.py`, `shifterware/tools/patch_image_crc.py`), which used two
different-but-equivalent encodings of the same MPEG-2 CRC. To stamp a boot
image, point a boot ware's `.bin` rule at the same script — no flags needed
(the format is auto-detected).

## `emulate_mm32f031.py`

Cortex-M0 boot-flow emulator for the MM32F031F6U6 (the eShifter MCU).
Built on Unicorn + Capstone, with hook-based peripheral stubbing.

**What it does**:
- Maps shifterboot.bin at `0x08000000` and shifterware.bin at
  `0x08003000`, plus an alias copy at `0x00000000` (the default boot
  remap target with `BOOT0=0`).
- Maps 4 KB of SRAM at `0x20000000`.
- Stubs out the peripheral regions with hooks that:
  - Return "HSI/HSE ready, FLASH idle, USART TX-empty" for status-poll
    reads, so the firmware's `while (!ready)` loops terminate.
  - Mirror `RCC_CFGR.SW` into `SWS` on every read, so the
    `set_sysclock_to_48m` poll exits.
  - Catch every memory write and log writes to interesting addresses
    (G_HCLK_HZ, SYSCFG_CFGR1, SysTick CTRL/LOAD, AIRCR).
- Short-circuits shifterboot's `mdelay()` spin at `0x080014CA` (which
  relies on the SysTick IRQ that Unicorn doesn't auto-fire) so
  shifterboot can make progress through its boot init.

**What it cannot do**:
- Run the live Modbus / UART protocol (no peer to send packets).
- Simulate Cortex-M0 IRQ delivery (we hook around the cases that block,
  but a full vector-dispatched IRQ model isn't here).
- Tell us what happens on the real eShifter PCB beyond the firmware's
  visible behaviour — observed in-emulator behaviour is necessary but
  not sufficient evidence for hardware semantics.

### Usage

```
python3 tools/emulate_mm32f031.py [options]

Options:
  --from {shifterboot,shifterware,shifterware-main}
        Entry point. Default: shifterboot (cold reset).
        shifterware       : reset via shifterware's vector slot 1
        shifterware-main  : skip Reset_Handler, jump straight to `main`
  --ware {oem,ours}       Default: oem. Use `ours` to load the locally-built
                          shifterware bin from `shifterware/build/`.
  --max-steps N           Default 200,000.
  --trace                 Print every instruction (first 200).
  --dump-sram-writes      Print the tail of SRAM-write log.
```

### Key findings produced by this tool

1. **shifterboot stays in its Modbus loop forever** when started from
   cold reset (BOOT0=0). Even after 1,000,000 simulated instructions,
   the PC is in shifterboot's main loop poll — it never branches into
   shifterware's region. Confirms shifterboot is a long-running OTA
   server, not a "validate + jump to app" loader.

2. **OEM shifterware traps in `boot_init_systick`** when entered
   without `G_HCLK_HZ` pre-set: the bounds check
   `(G_HCLK_HZ / 1000) - 1 <= 0x00FFFFFF` fails (the result is
   `0xFFFFFFFF`), and the OEM falls into a `b .` loop at PC
   `0x080042CA`. Confirmed by emulation — only 55 memory writes
   recorded before the trap, no Modbus or motor activity. Therefore
   `G_HCLK_HZ` on the real eShifter PCB is set by some out-of-band
   mechanism (production-flow programming, SRAM retention, or a
   shifterboot code path the static xref scan doesn't catch).

3. **Our build boots successfully.** With the explicit
   `G_HCLK_HZ = 48000000u` self-init we added on 2026-05-19, the
   emulator runs through ≥500,000 instructions without trapping,
   logs 43,000+ SRAM writes (the super-loop iterating), and settles
   inside the main poll loop. Independent validation that our
   decomp + functional-firmware patches actually boot.

4. **`SYSCFG_CFGR1.MEM_MODE = 3` happens early in shifterware's
   boot** — confirmed at step 280 of OEM execution / step 330 of
   our build (right after the .data memcpy, before `cpsie i`). Maps
   SRAM to `0x00000000` per UM § "SYSCFG configuration register"
   — Cortex-M0 vector-table relocation mechanism.

Run with `--trace` to see the first 200 instructions executed; useful
when investigating a new region of code.
