# mainboot — decomp progress

Target binary: `muco-boot.bin` (32768 bytes, ARM Cortex-M3+, STM32F4/F7
class, 320 KB SRAM). Loaded into Ghidra at `0x08000000`. The bootloader
sits at the base of flash and (presumably) jumps to the application at
some higher offset once an integrity check passes.

See `docs/hardware.md` for the canonical binary identity (size, SHA
hashes) and the MCU identification work.

## Decomp scope policy

Mirroring the shifterboot rule: **decode only VanMoof-custom code.**
Functions that are byte-for-byte copies of canonical vendor sources
(ST CMSIS, ST HAL/LL, ARM CMSIS-Core) are *recognised* and marked
`vendor-stock` — no separate C translation, no source file in
`src/`. The byte-equivalent build will later pull these from a
vendored copy of the ST library; the decomp work focuses on the
bespoke parts of the bootloader.

## Summary

| Count | Status |
| --- | --- |
| ?? | pending (awaiting first dump) |
| 0  | vendor-stock (recognised) |
| 0  | in-progress |
| 0  | decomp (asm or c) |
| 0  | named (rename in Ghidra, no source yet) |

`function_count = ??` per `ghidra/exports/mainboot_program.json`
(refresh after every mutating Ghidra run; see top-level CLAUDE.md).

## Per-module decomp log

_None yet — scaffold just created._

## Functions

_Inventory pending the first Ghidra dump. Will populate two tables
once classification begins:_

- _Vendor-stock (recognised, no decomp needed)_
- _VanMoof-custom (decomp targets)_

Full list will live in `ghidra/exports/mainboot_program.json`.

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
