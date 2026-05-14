# vanmoof-s3-decomp

Clean-room rebuild of **VanMoof S3 / X3 firmwares** from decompilations,
producing a buildable C source tree that re-emits binary-equivalent (or
behaviour-equivalent) images.

**Active targets:**

| Subdir | Firmware | MCU | Role | Size | Status |
| --- | --- | --- | --- | --- | --- |
| [`shifterware/`](shifterware/) | `shifterware 0.237` | MM32F031F6U6 (Cortex-M0) | e-Shifter application (super-loop + Modbus bus protocol) | ~12 KB | active — `59 decomp-c / 6 named / 64 pending` |
| [`shifterboot/`](shifterboot/) | `shifterboot` (unversioned) | MM32F031F6U6 (Cortex-M0) | e-Shifter first-stage bootloader at flash base | 6 KB | active — `4 named+asm / 74 pending` |
| [`mainboot/`](mainboot/) | `muco-boot` (unversioned) | ST STM32F413VGT6 (Cortex-M4) | main-controller bootloader (third-party Muco Technologies) | 32 KB | active — `7 decomp-c / 1 decomp-asm / 1 vendor-stock / 1 named / 170 pending` |
| [`mainware/`](mainware/) | `mainware 1.07.06` | ST STM32F413VGT6 (Cortex-M4) | main-controller application (BLE, modem, kick-lock, sound, power state, subsystem updaters) | ~213 KB | active — `6 decomp-c / 3 vendor-stock / 8 named / 794 pending` |

**Shifterboot lineage:** the bootloader's startup path is a lightly-customised
MindMotion vendor template — `Reset_Handler`, the Cortex-M0 vector table, and
`SystemInit` are byte-identical to MindMotion's stock
[`startup_MM32F031x4x6_q.s`](https://github.com/SoCXin/MM32F031/blob/master/src/device/MM32F031x4x6_q/Source/KEIL_StartAsm/startup_MM32F031x4x6_q.s)
and
[`system_MM32F031x4x6_q.c`](https://github.com/SoCXin/MM32F031/blob/master/src/device/MM32F031x4x6_q/Source/system_MM32F031x4x6_q.c)
from the pre-2021 MindMotion AE-Team SDK (same source Keil's
[`MM32F031_DFP`](https://www.keil.arm.com/packs/mm32f031_dfp-mindmotion/devices/)
ships). The MindMotion BSP itself is a fork of ST's `system_stm32f10x.c`
template — which is why the RCC reset masks visible in the decomp
(`0xF0FF0000`, `0xFFF6FFFF`, `0xFFFBFFFF`, `0x009F0000`) are F1-lineage rather
than F0-native. The bespoke parts are a custom 20-byte cold-reset stub that
replaces the Keil/ARMCC `__main` C runtime — packed into the unused tail of
the Cortex-M0 vector table at file offset `0xB4` — and `SysTick_Handler` (the
stock template has `B .` there; VanMoof installed real logic). See
[`shifterboot/docs/progress.md`](shifterboot/docs/progress.md) for the
per-function evidence.

**Per-firmware MCU mapping** (loader + application share an MCU):

| Firmware | MCU | Family |
| --- | --- | --- |
| `mainware` / `muco-boot`     | ST STM32F413VGT6        | Cortex-M4, 1 MB flash, 320 KB SRAM |
| `bleware` / `bleboot`        | TI CC2642R1F            | Cortex-M4F, BLE 5.2 |
| `motorware`                  | TI TMS320F28054F        | C2000 Piccolo (32-bit DSP — **not ARM**) |
| `shifterware` / `shifterboot`| MindMotion MM32F031F6U6 | Cortex-M0 (STM32F031 clone) |
| `batteryware` / `bmsboot`    | ST STM32L072CZT6        | Cortex-M0+, 192 KB flash, 20 KB SRAM |

The remaining wares (mainware, bleware, bleboot, motorware,
batteryware, bmsboot) come later under the same project; they live in
sibling subdirectories once started.

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
