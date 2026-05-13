# vanmoof-s3-decomp

Clean-room rebuild of **VanMoof S3 / X3 firmwares** from decompilations,
producing a buildable C source tree that re-emits binary-equivalent (or
behaviour-equivalent) images.

**First target: the e-Shifter** (`shifterware 0.237`, SHA-512 `8f454dfc…97409`),
running on the **MM32F031F6U6** Cortex-M0 MCU on the shifter PCB. Other
firmwares (motorware, batteryware, mainware, bleware) come later under the
same project; they live in sibling subdirectories once started.

| | |
| --- | --- |
| MCU | MIndMotion **MM32F031F6U6** (LQFP-20) — ARM Cortex-M0 @ 48 MHz |
| Flash | 32 KB (firmware uses ~12 KB of it) |
| SRAM  | 4 KB |
| Datasheet | <https://crossic.com/wp-content/uploads/2021/12/DS_MM32F031xx_EN.pdf> |

## Target firmware

| Field | Value |
| --- | --- |
| Filename | `shifterware_0.237.bin` |
| Version word | `0x00ED02C1` (decimal `0.237` per VanMoof scheme) |
| Length | `0x2EA8` bytes (≈11 944) |
| CRC | `0x1E8EB125` (CRC-32, VanMoof poly — see `vanmoof/crc.go`) |
| Build date | `Oct 23 2020 14:09:11` |
| SHA-512 | `8f454dfc1e600dfeae772465dd9791cde1b7588be22f7d88e88e61c9708634173a730be85ba19214d6c4544576ebeb8ea7e51e2686a163b77f7693292da97409` |

This is the *first* (oldest) shipped shifterware version — chosen as the baseline
because later versions are minor variants and the diff is small.

## Repository layout

```
vanmoof-s3-decomp/
├── README.md            ← you are here
├── Makefile             ← arm-none-eabi-gcc build
├── linker_mm32f031.ld   ← memory map for MM32F031F6U6
├── include/
│   ├── mm32f031.h       ← MMIO base addresses, register layout
│   ├── compiler.h       ← __attribute__ helpers
│   └── shifter.h        ← project-wide types / constants
├── src/
│   ├── startup_mm32f031.S   ← vector table + reset entry
│   ├── system_mm32f031.c    ← clock setup, low-level init
│   ├── main.c               ← application entry
│   └── …                    ← one file per logical module
├── docs/
│   ├── memory-map.md
│   ├── progress.md          ← per-function decomp status
│   ├── decomp-workflow.md   ← how a function moves from FUN_xxx → C
│   └── peripherals.md       ← which MMIO blocks the shifter touches
├── ghidra/
│   ├── scripts/             ← project-specific Ghidra helpers
│   └── exports/             ← JSON dumps from Ghidra (functions, vectors, strings)
├── reference/               ← pin map, datasheet excerpts, third-party notes
└── build/                   ← arm-none-eabi-gcc output (gitignored)
```

## Building

You need an ARM Cortex-M0 cross toolchain and `make`:

```bash
# Debian / Ubuntu
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi

# macOS
brew install --cask gcc-arm-embedded

# Build
make                    # produces build/shifterware.elf and build/shifterware.bin
make compare            # diff our build vs the OEM shifterware_0.237.bin
make clean
```

The `compare` target verifies how close our reconstruction is to the OEM image —
the goal is byte-equivalence, with documented divergences acceptable in early
iterations.

## Development workflow

1. Open Ghidra (`vanmoof.gpr` in `~/`) and load `shifterware_0.237.bin`.
2. Run the helper script
   [`ghidra/scripts/DumpProgramInfo.java`](ghidra/scripts/DumpProgramInfo.java)
   from Ghidra's *Script Manager* — outputs a JSON snapshot to
   `ghidra/exports/shifter_program.json`.
3. Pick a `FUN_*` function from `docs/progress.md`. Mark `in-progress`.
4. Use the Ghidra to:
   - decompile, disassemble, set prototypes, rename variables
   - rename `FUN_xxxxxxxx` → meaningful name once understood
5. Translate decompiler output to a hand-written C function in the appropriate
   `src/*.c` file. Match the OEM ABI (parameters, return type).
6. Commit. Update `docs/progress.md` with the new status and any insights.

See [`docs/decomp-workflow.md`](docs/decomp-workflow.md) for the full process,
naming conventions, and quality gates.

## Legal

This is a clean-room interoperability project. The OEM binary is **not**
redistributed in this repository — you must extract `shifterware_0.237.bin`
from a `.pak` file or SPI flash dump of a bike you own.

Reverse engineering for interoperability is permitted under EU Software
Directive 2009/24/EC Art. 6 and the US DMCA §1201(f). The reconstructed source
in this repository is original work derived from analysis of the OEM image and
publicly available documentation (datasheets, reference manuals).

The author makes no warranty. Flashing reconstructed firmware to a bike will
**brick or damage hardware** if it is wrong. Do not flash to a bike you depend
on. Use a spare shifter PCB or hardware-in-the-loop simulator.

## Contributing

PRs welcome. 
