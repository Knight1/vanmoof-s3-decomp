# mainboot — decomp progress

Target binary: `muco-boot.bin` (32768 bytes, ARM Cortex-M4, STM32F469/F479,
320 KB SRAM). Loaded into Ghidra at `0x08000000`. The image is a
third-party **Muco Technologies** STM32F4 bootloader (banner
`"STM32 bootloader <%X.%02X> Muco Technologies (c)2019"`) that VanMoof
licensed for the main-controller first-stage. It manages multiple
firmware slots (Loaded / Shadow / Shifter / Motor / Battery
Application) with CRC-verified A/B updates.

See `docs/hardware.md` for the canonical binary identity (size, SHA
hashes), the MCU identification work, and the Muco provenance note.

## Decomp scope policy

The shifterboot rule ("decode only VanMoof-custom code") does
**not** apply cleanly here. The whole binary is third-party (Muco
Technologies) — there is no VanMoof-custom inner core to focus on.
We still want a buildable reconstruction, so the working policy is:

- **Translate every function** that has observable behaviour
  (skip 2-byte `b .` trap stubs — they get listed as
  `decomp-asm`, one shared source line).
- **Recognise ST CMSIS / HAL / LL** stock functions when they
  appear (they will — Muco almost certainly used STM32 CubeF4 HAL
  underneath their loader). Mark those `vendor-stock` and let the
  build pull them from a vendored Cube tree later.
- **Surface, but do not separately re-implement, ARM CMSIS-Core
  intrinsics** (`__DSB`, `__ISB`, `NVIC_SystemReset`, etc.) —
  these will be `vendor-stock` like in shifterboot.

The bespoke layer worth understanding deeply is the **image-table
format and integrity scheme**: what the loaded/shadow/per-subsystem
slots look like in flash, what fields the version banners are
reading, what CRC polynomial+seed the integrity check uses, and
how mainboot hands per-subsystem blobs off (probably over Modbus
to the eShifter/motor/battery MCUs).

## Summary

| Count | Status |
| --- | --- |
| 178 | pending (Muco — awaiting decomp) |
| 0   | vendor-stock (recognised; ST/ARM CMSIS — none confirmed yet) |
| 0   | in-progress |
| 2   | decomp-c |
| 0   | named (rename in Ghidra, no source yet) |

`function_count = 180` per `ghidra/exports/mainboot_program.json`
(refresh after every mutating Ghidra run; see top-level CLAUDE.md).

## Per-module decomp log

- `rcc.c` — `rcc_reset_all_peripherals` and the empty
  `rcc_post_reset_hook` it calls. Resets every peripheral on every
  bus (APB1 → APB2 → AHB1 → AHB2 → AHB3) by writing
  `0xFFFFFFFF` then `0` to each `*RSTR`. Returns 0.

## Functions

### Decoded

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x080005d0` | 2  | `rcc_post_reset_hook`       | `src/rcc.c` | empty `bx lr`; placeholder hook Muco never filled in |
| `0x080005d4` | 38 | `rcc_reset_all_peripherals` | `src/rcc.c` | pulse-resets all peripherals via the five RCC `*RSTR` registers, returns 0 |

### Pending decomp targets (small leaves to look at next)

| Address | Size | Notes |
| --- | --- | --- |
| `0x08000204` | 16 | guard-flag check (`if (*flag) { … nop.w }`) — body is a `nop.w`; likely a stubbed-out logging helper |
| `0x08000520` | 16 | `strlen`-style loop (`while (*p++); return p - start - 1`) |
| `0x0800067c` | 14 | `*(uint32_t*)A += *(uint8_t*)B` — looks like a small counter increment |
| `0x08000694` | 6  | trivial getter `return *(uint32_t*)lit` |
| `0x080006a0` | 34 | (TBC) |
| `0x080006c8` | 32 | (TBC) |

Full list in `ghidra/exports/mainboot_program.json` (178 still
auto-named `FUN_xxxxxxxx`).

## Open questions

- Exact MCU part (F4 F469/F479 vs F7 F745/F746/F756)?
- Where does mainboot jump to the application image? (`0x08008000`
  guessed from STM32 F4 sector layout — confirm.)
- Integrity scheme (CRC32? SHA? signature?)
- OTA delivery path — does mainboot accept image uploads itself, or
  does it leave that to the application?
- Why is the initial SP at `0x20050000` while `.bss` ends at
  `0x20000950`? — i.e., what is the upper ~315 KB of SRAM used for?
  Stack growth alone is unlikely to need that much; probably an
  image staging area used by the upload path.
- Is there a USB DFU or YMODEM path baked into the bootloader?
