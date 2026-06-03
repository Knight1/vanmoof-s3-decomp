# powerbankboot

Decompilation of the **VanMoof PowerBank bootloader**
(`powerbank_bootloader_1.00.bin`) — the 32 KB loader that owns the base of flash
on the external range-extender battery and validates / installs / launches the
[`../powerbankware`](../powerbankware) application.

- **MCU:** STM32F091xC (ARM Cortex-M0, 256 KB flash, 32 KB SRAM) — same part as
  powerbankware
- **Boot base:** `0x08000000` (first 32 KB of flash; the CPU resets straight
  into this image's vector table — there is **no** VanMoof image header on the
  loader itself)
- **Identity:** the banner string is `"\nI am VM-BATT BL\r"` — this is the
  **VanMoof battery-module bootloader**, shared with `bmsboot`. The PowerBank is
  a BMS sibling of the in-frame battery, so it carries the same loader.

## What it does

A robust **A/B (dual-bank) bootloader** with an ST IEC-60730 Class-B self-test
library (X-CUBE-STL) stitched in. On reset it:

1. brings up the clock / IWDG / RTC / UART / GPIO,
2. reads a persisted *upgrade-finished* flag from the **RTC backup registers**,
3. validates the two 112 KB image banks (**AP** `0x08008000`, **Shadow**
   `0x08024000`) by magic + size + CRC-32, and
4. installs the freshest valid image (copying **Shadow → AP** after an OTA, or
   re-mirroring **AP → Shadow** as a backup), then **jumps to the application**
   at `0x08008000 + 0x28`.

If neither bank is valid it stays resident and runs a small **serial download
server** (a `"Who?"` handshake + command/complement + XOR-checked data blocks)
to reflash the AP bank over USART2.

```
flash:  0x08000000 ┌─────────────────────┐ powerbankboot (this, 32 KB)
                   ├─────────────────────┤ 0x08008000
        AP bank    │  application (112KB)│  ← powerbankware runs here
                   ├─────────────────────┤ 0x08024000
      Shadow bank  │  backup copy (112KB)│
                   └─────────────────────┘ 0x08040000
```

## Layout

```
powerbankboot/
├── Makefile                 # build (arm-none-eabi-gcc, cortex-m0)
├── linker_stm32f091.ld      # loader @ 0x08000000, 32 KB window, 32 KB SRAM
├── src/
│   ├── startup_stm32f091.S  # M0 vector table (at flash base) + Reset stub + .data/.bss init
│   ├── main.c               # X-CUBE-STL main template with the boot_main() call spliced in
│   ├── boot.c               # boot_main() A/B orchestrator + goto_application()
│   ├── image.c              # image_verify() (magic+size+CRC32) + flash_copy_image()
│   ├── flash.c              # flash erase / program / read + mem_copy
│   ├── ota.c                # "Who?" serial-download protocol byte machine
│   ├── uart.c               # RX/TX ring drain + pump + flush + USART2 ISR
│   ├── system.c             # clock / IWDG / RTC-backup / GPIO bring-up
│   ├── handlers.c           # NMI(CSS)/HardFault/SVC/PendSV/SysTick + STL clock-meas ISR
│   └── strings.c            # flash banner / trace strings
├── include/powerbankboot.h  # prototypes, register + memory-map defines
├── docs/                    # memory-map, hardware, protocol, progress
└── ghidra/exports/          # powerbankboot_program.json (Ghidra snapshot)
```

## Decomp scope

Per the project policy, only the **VanMoof-custom** loader code is translated;
the bulk of the 32 KB image is recognised vendor code and left as `vendor-stock`
externs:

- **ST X-CUBE-STL** (Class-B self-test library): CPU/RAM/Flash-CRC/clock tests,
  the control-flow signature counters, and `FailSafe`. The OEM spliced the
  bootloader call (`boot_main()`) into the STL `main()` right after log init, so
  on a normal boot the loader runs first and the self-test body is dormant.
- **STM32F0 HAL** (RCC / FLASH / RTC / UART / GPIO / IWDG).
- A small **tinyprintf** formatter behind `dbg_printf()`.

See `docs/progress.md` for the per-function status and the full control-flow map.

> Decomp discipline: C is derived from **this** binary's own disassembly;
> `bmsboot` is used only as confirmation for the shared battery-loader core.
