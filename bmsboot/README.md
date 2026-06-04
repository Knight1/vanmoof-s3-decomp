# bmsboot

Decompilation of the **VanMoof battery-module (BMS) bootloader**
(`bmsboot_v007.bin`) — the 20 KB loader that owns the base of flash on the
in-frame S3/X3 battery and validates / installs / launches the
[`../batteryware`](../batteryware) application.

- **MCU:** STM32L072CZT6 (ARM Cortex-M0+, 192 KB flash, 20 KB SRAM, 6 KB EEPROM)
  — same part as `batteryware`
- **Boot base:** `0x08000000` (first 20 KB of flash; the CPU resets straight into
  this image's vector table — there is **no** VanMoof image header on the loader)
- **Identity:** the banner is `"\nI am VanMoof BL V007 2022-11-04 09:32:30\r"` — the
  "VanMoof BL" battery-module loader, the STM32L0 sibling of the STM32F091
  PowerBank loader [`../powerbankboot`](../powerbankboot)

## What it does

An **A/B (dual-bank) bootloader** for the BMS. On reset it:

1. relocates its vector table into SRAM (sets `VTOR`), brings up the clock / CRC /
   IWDG / GPIO, and persists the reset cause to EEPROM,
2. reads a persisted **boot flag** from data EEPROM (`0x08080000`),
3. validates the application bank (**AP** `0x08005000`) by magic + size + CRC-32,
   recovering it from the **Shadow** bank (`0x0801A800`) when AP is corrupt or an
   OTA just finished, and
4. **jumps to the application** at `0x08005000 + 0x28`.

If asked (a boot-flag value, or PA10 asserted at reset) it stays resident and runs
a small **serial-download server** (a `"WHO?"` handshake + command/complement +
XOR-checked 128-byte data blocks) to reflash the AP bank over USART1 @ 9600.

```
flash:  0x08000000 ┌─────────────────────┐ bmsboot (this, 20 KB)
                   ├─────────────────────┤ 0x08005000
        AP bank    │  application (86 KB)│  ← batteryware runs here
                   ├─────────────────────┤ 0x0801A800
      Shadow bank  │  backup copy (86 KB)│
                   └─────────────────────┘ 0x08030000
EEPROM: 0x08080000  boot flag (+0)  ·  reset cause (+2)
```

## Layout

```
bmsboot/
├── Makefile                 # build (arm-none-eabi-gcc, cortex-m0plus)
├── linker_stm32l072.ld      # loader @ 0x08000000, 20 KB window, 20 KB SRAM
├── src/
│   ├── startup_stm32l072.S  # M0+ vector table (at flash base) + Reset + .data/.bss + libc init
│   ├── main.c               # boot decision + resident super-loop
│   ├── boot.c               # goto_application()
│   ├── image.c              # image_verify() (magic+size+CRC32) + flash_copy_image()
│   ├── flash.c              # flash erase / program / read + mem helpers
│   ├── ota.c                # "WHO?" serial-download protocol byte machine
│   ├── uart.c               # USART1 RX/TX rings + ISR + bring-up/teardown
│   ├── system.c             # clock / CRC / IWDG / GPIO / SysTick bring-up
│   ├── handlers.c           # HardFault -> failsafe -> system_reset + SysTick pacer
│   └── strings.c            # flash banner strings
├── include/bmsboot.h        # prototypes, register + memory-map defines
├── docs/                    # memory-map, hardware, protocol, progress
└── ghidra/                  # gen_program_json.py + exports/bmsboot_program.json
```

## Decomp scope

Per the project policy, only the **VanMoof-custom** loader code is translated; the
leaf drivers are recognised vendor code, left as named externs / `vendor-stock`:

- **STM32L0 HAL** (RCC / FLASH / GPIO / CRC / IWDG / UART).
- **CMSIS / libgcc runtime** (NVIC, SysTick, `aeabi_*` division, `clz`).

See `docs/progress.md` for the per-function status and the full control-flow map.

## Build

```
make            # compiles src/ clean under -Wall -Wextra -Wpedantic -Wshadow
make compare OEM_IMAGE=../../VanMooof-Firmware/ES3/bmsboot/bmsboot_v007.bin
```

The image does not link standalone yet: the HAL/runtime leaves are `extern` and
resolve once those libraries are vendored in (same state as `../powerbankboot`).

> Decomp discipline: C is derived from **this** binary's own disassembly;
> `powerbankboot` is used only as confirmation for the shared battery-loader core.
