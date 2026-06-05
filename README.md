# vanmoof-s3-decomp

Clean-room rebuild of **VanMoof S3 / X3 firmwares** from decompilations,
producing a buildable C source tree that re-emits binary-equivalent (or
behaviour-equivalent) images.

## Active targets

| Subdir | Firmware | MCU | Size | Status |
| --- | --- | --- | --- | --- |
| [`shifterware/`](shifterware/) | `shifterware 0.237` | MM32F031F6U6 (Cortex-M0) | ~12 KB | `59 decomp / 6 named / 64 pending` |
| [`shifterboot/`](shifterboot/) | `shifterboot` | MM32F031F6U6 (Cortex-M0) | 6 KB | `4 named+asm / 74 pending` |
| [`mainboot/`](mainboot/) | `muco-boot` | STM32F413VGT6 (Cortex-M4) | 32 KB | `7 decomp / 2 named / 170 pending` |
| [`mainware/`](mainware/) | `mainware 1.07.06` | STM32F413VGT6 (Cortex-M4) | ~213 KB | `6 decomp / 11 named / 794 pending` |
| [`bleboot/`](bleboot/) | `bleboot 1.0.0` | TI CC2642R1F (Cortex-M4F) | 8 KB | **complete** — byte-equivalent, Unicorn-validated |
| [`bleware/`](bleware/) | `bleware 1.4.01` | TI CC2642R1F (Cortex-M4F) | 178 KB | **159 decompiled** — OAD header byte-equivalent, full VanMoof code compiles |
| [`batteryware/`](batteryware/) | `batteryware 1.17.1` | STM32L072CZT6 (Cortex-M0+) | ~88 KB | all functions `decomp-c` — builds (40 KB) + self-CRCs; behaviour-faithful (byte-eq out of reach: toolchain) |
| [`powerbankware/`](powerbankware/) | `powerbank_firmware 1.11.05` | STM32F091xC (Cortex-M0) | ~92 KB | early — scaffold + `main`/state-loop decompiled (compiles; not yet linking) |
| [`motorware/`](motorware/) | `motorware S.0.00.22` | TI TMS320F28054F (C2000/C28x DSP — **not ARM**) | ~58 KB flash | foundation verified — container byte-exact round-trip, boot flow + memory map proven, `.cinit` decoded, **356 functions disassembled** (IDA C28x); per-function C is open |

## Per-firmware MCU mapping

| Firmware | MCU | Family |
| --- | --- | --- |
| `mainware` / `muco-boot` | ST STM32F413VGT6 | Cortex-M4, 1 MB flash, 320 KB SRAM |
| `bleware` / `bleboot` | TI CC2642R1F | Cortex-M4F, BLE 5.2, 352 KB flash, 80 KB SRAM |
| `motorware` | TI TMS320F28054F | C2000 Piccolo (32-bit DSP — **not ARM**) |
| `shifterware` / `shifterboot` | MindMotion MM32F031F6U6 | Cortex-M0 (STM32F031 clone) |
| `batteryware` / `bmsboot` | ST STM32L072CZT6 | Cortex-M0+, 192 KB flash, 20 KB SRAM |
| `powerbankware` / `powerbankboot` | ST STM32F091xC | Cortex-M0, 256 KB flash, 32 KB SRAM (PowerBank BMS — no VTOR, SYSCFG SRAM remap) |

## Repository layout

```
vanmoof-s3-decomp/
├── README.md
├── Makefile                ← top-level dispatcher (make shifterware, make bleware, …)
├── .gitignore
├── shifterware/            ← e-Shifter application
├── shifterboot/            ← e-Shifter bootloader
├── mainboot/               ← main-controller bootloader
├── mainware/               ← main-controller application
├── bleboot/                ← BLE-MCU bootloader (complete)
├── bleware/                ← BLE-MCU application (active)
│   ├── src/                ← 47 .c files — all VanMoof-custom code
│   ├── include/            ← bleware.h, cc2642r1.h, monitor.h
│   ├── vendor/             ← fetch_sdk.sh + README (TI SDK integration)
│   ├── docs/               ← progress.md, hardware.md
│   ├── ghidra/exports/     ← bleware_program.json
│   └── linker_cc2642r1.ld
├── batteryware/            ← in-frame battery BMS (STM32L072, all functions decomp-c)
├── powerbankware/          ← PowerBank BMS application (STM32F091, active — early)
├── tools/                  ← cross-ware tools (patch_image_header.py, emulator, modbus)
└── reference/              ← shared datasheets, pin maps
```

## Building

### bleware

```bash
cd bleware

# Build with weak stubs (no TI SDK needed):
make

# Build with TI SDK headers (requires SDK 3.40.00.02):
./vendor/fetch_sdk.sh ~/Downloads/simplelink_cc13x2_26x2_sdk_3_40_00_02.run
make TI_SDK=vendor/ti-sdk-3.40

# Byte-level comparison against OEM firmware:
make compare OEM_IMAGE=path/to/bleware_1.4.01.bin
```

The OAD image header (144 bytes at flash 0x00-0x8F) is byte-equivalent to the
OEM binary. The remaining ~180 KB requires the full TI BLE stack compiled in
Code Composer Studio with the SimpleLink SDK 3.40 project.

### shifterware

```bash
cd shifterware
make            # produces build/shifterware.elf and build/shifterware.bin
make compare    # diff vs OEM shifterware_0.237.bin
```

## Development workflow

1. Open Ghidra (`vanmoof.gpr`) and load the target firmware binary.
2. Pick a `FUN_*` function from `docs/progress.md`. Mark `in-progress`.
3. Decompile, disassemble, set prototypes, rename variables in Ghidra.
4. Translate decompiler output to C in the appropriate `src/*.c` file.
   Match the OEM ABI exactly (parameters, return type, control flow).
5. Build (`make`) — must compile with no warnings (`-Wall -Wextra`).
6. Compare (`make compare`) — note how the translation affects the diff.
7. Update `docs/progress.md`, `docs/hardware.md`, `docs/protocol.md`.
8. Update `ghidra/exports/<ware>_program.json` with renamed functions.

See `CLAUDE.md` for the full conventions, naming rules, and Ghidra hygiene.

## Legal

This is a clean-room interoperability project. OEM firmware images are **not**
redistributed in this repository — you must extract them from a `.pak` file
or SPI flash dump of a bike you own.

Reverse engineering for interoperability is permitted under EU Software
Directive 2009/24/EC Art. 6 and US DMCA §1201(f). The reconstructed source
in this repository is original work derived from analysis of the OEM image
and publicly available documentation.

The author makes no warranty. Flashing reconstructed firmware to a bike will
**brick or damage hardware** if it is wrong. Do not flash to a bike you depend
on. Use a spare PCB or hardware-in-the-loop simulator.
