# powerbankware

Decompilation of the **VanMoof PowerBank** firmware
(`powerbank_firmware_1.11.05.bin`) — the external range-extender battery that
feeds the bike.

- **MCU:** STM32F091xC (ARM Cortex-M0, 256 KB flash, 32 KB SRAM)
- **App base:** `0x08008000` (above the 32 KB `powerbankboot`)
- **Identity:** VanMoof image, version `0x011105B2` (1.11.05, type 0xB2)

It is a close sibling of [`../batteryware`](../batteryware): the same FEDL5236
BMS core (SOC/SOH, protections, calibration, telemetry, shipping mode), plus a
**power-path output stage** the in-frame battery lacks — bypass FET, charger/
load detection, and a DAC-regulated ~20–30 V output rail.

`powerbankboot` (the 32 KB loader at `0x08000000`) is a **separate** target and
will get its own folder; nothing from it belongs here.

## Layout

```
powerbankware/
├── Makefile                 # build (arm-none-eabi-gcc, cortex-m0)
├── linker_stm32f091.ld      # app @ 0x08008000, 32 KB SRAM
├── src/
│   ├── startup_stm32f091.S  # VanMoof header + M0 vector table + Reset_Handler
│   ├── main.c               # main() — boot pre-check + state-machine super-loop
│   └── strings.c            # flash log/banner strings
├── include/powerbankware.h  # prototypes
├── docs/                    # memory-map, hardware, progress, protocol
└── ghidra/exports/          # powerbankware_program.json (Ghidra dump)
```

See `docs/progress.md` for the boot/control-flow map and the per-function
status. Build artifacts are stamped by the shared `../tools/patch_image_header.py`.

## Status

Early. `main` (the boot pre-check + 28-way state-machine super-loop) and its
banner strings are reconstructed and compile clean (`-Wall -Wextra -Wpedantic
-Wshadow`). The image does **not** link yet — the per-state routines and the
support functions `main` calls are still extern stubs, landing incrementally.

## Dumping the program JSON

The per-ware Ghidra script `DumpPowerbankwareProgram.java`
(`~/ghidra_scripts/`, mirrored to `/mnt/c/Users/Tobias/ghidra_scripts/`) writes
`ghidra/exports/powerbankware_program.json` — the function/string/vector
snapshot the tracker is kept in sync with. Run it after any renaming pass, with
the program's image base set to **`0x08008000`** (Script Manager → *VanMoof →
DumpPowerbankwareProgram*, or headless `-postScript`).

> Decomp discipline: C is derived from **this** binary's own disassembly;
> batteryware is used only as confirmation for the shared BMS code.
