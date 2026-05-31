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
├── src/startup_stm32f091.S  # VanMoof header + M0 vector table + Reset_Handler
├── include/powerbankware.h  # seed prototypes
└── docs/                    # memory-map, hardware, progress, protocol
```

See `docs/progress.md` for the boot/control-flow map and the per-function
status. Build artifacts are stamped by the shared `../tools/patch_image_header.py`.

> Decomp discipline: C is derived from **this** binary's own disassembly;
> batteryware is used only as confirmation for the shared BMS code.
