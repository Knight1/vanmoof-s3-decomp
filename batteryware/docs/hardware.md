# batteryware — hardware notes

The battery management firmware on the VanMoof S3 sits on an
**ST STM32L072CZT6** — a Cortex-M0+ ultra-low-power MCU with 192 KB
flash and 20 KB SRAM.

| | |
| --- | --- |
| MCU | ST STM32L072CZT6 (Cortex-M0+) |
| Core | ARM Cortex-M0+, 32 MHz max |
| Flash | `0x08000000..0x0802FFFF` (192 KB) |
| SRAM | `0x20000000..0x20004FFF` (20 KB) |
| Vector table | flash `0x08000028` (40-byte VanMoof header before VT), 48 entries (16 system + 32 IRQ) |

## Binary identity

| Field | Value |
| --- | --- |
| Filename | `batteryware_1.17.1.bin` |
| Version word | `0x011701B1` (MAJOR=0x01, MINOR=0x17=23, PATCH=0x01, TYPE=0xB1=batteryware) |
| Size | 87,568 bytes (`0x15610`) |
| CRC | `0x2E0150DA` (CRC-32 MPEG-2, VanMoof poly) |
| Build date | TBD (embedded in VanMoof header at `+0x10`) |

## Image header (40 B at file offset 0)

Standard VanMoof 40-byte header at the start of the image:

| Offset | Bytes | Value | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | `0xAA55AA55` | Magic |
| `+0x04` | 4 | `0x011701B1` | Version word |
| `+0x08` | 4 | `0x2E0150DA` | CRC32 over image (this field + length blanked) |
| `+0x0C` | 4 | `0x00015610` | imageSize |
| `+0x10` | 12 | build date (ASCII) | |
| `+0x1C` | 9 | build time (ASCII) | |
| `+0x24` | 4 | `0xFFFFFF00` | Padding |

## Memory map

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (batteryware) | `0x08000000` | `0x0801560F` | firmware image (87.5 KB of 192 KB flash) |
| SRAM              | `0x20000000` | `0x20004FFF` | 20 KB |
| Peripherals       | `0x40000000` | `0x5FFFFFFF` | APB1 / APB2 / AHB |
| Cortex-M0+ SCS    | `0xE000E000` | `0xE000EFFF` | NVIC, SysTick, SCB |

Initial SP from vector slot 0: `0x20005000` (top of SRAM).

## Confirmed GPIO pins

| Pin | Direction | Role | Source |
| --- | --- | --- | --- |
| PA4 (bit 0x10) | Output | **LED driver.** Toggled by `led_flash` at `0x0800527C` via `gpio_bit_write(GPIOA, 0x10, 1/0)`. High time is 100ms or 20ms depending on `g_pFastModeLed` SRAM flag; low time is 50ms or 10ms. | `led_flash` @ `0x0800527C` |

## SRAM globals

Addresses resolved from literal pool entries in the flash image.

| Address | Name (Ghidra) | Size | Description |
| --- | --- | --- | --- |
| `0x20000E70` | — | 4 | Runtime IRQ vector target (IRQ 25 → loaded from VT slot 41) |
| `0x2000199C` | — | 4 | Runtime SysTick callback pointer (VT slot 15 → `0x2000199D`) |
| `0x20001AA0` | — | 4 | Runtime IRQ vector target (IRQ 27 → loaded from VT slot 43) |
| `0x20002BFC` | — | 1 | `g_pFastModeLed` target — boolean flag controlling LED flash speed. `0` = slow (100ms/50ms), non-zero = fast (20ms/10ms). Read by `led_flash`. |
| `0x20002C10` | — | 4 | SysTick reload value holder — written by `delay_ms` ISR path. Initial value `0x0000AAAA`. |
| `0x20002C80` | — | 4 | SysTick CTRL register shadow or timer status port — polled by `delay_ms` busy-wait loop. Bit 0 = COUNTFLAG, bit 1 = overflow flag. |

## Peripheral usage

| Peripheral | Base | Confirmed usage |
| --- | --- | --- |
| GPIOA | `0x50000000` | LED output on PA4 (BSRR/BRR via `gpio_bit_write`) |
| GPIOB | `0x50000400` | Likely PB9 for charge on/off (per string `"\nChargeOn_Off() --> PB9=0\r"`) |
| USART1 | `0x40013800` | Serial comm with main module (IRQ 12 = USART1 at VT slot 28 → `0x080054C5`) |
| TIM2 | `0x40000000` | IRQ 5 and 7 assigned (two TIM2 IRQs) |
| SysTick | `0xE000E010` | 1 ms tick timer — polled by `delay_ms` |
| I2C1 | `0x40005400` | Likely FEDL5236 fuel gauge communication |

## Key observations

1. **FEDL5236 fuel gauge** — The strings reference `FEDL5236_Initialize()`,
   `FEDL5236_POWER_DOWN`, `FEDL5236_Max_Cell_Voltage`, `FEDL5236_Min_Cell_Voltage`,
   `FEDL5236_Total_Voltage`. This is a **Fortior Tech FEDL5236** battery
   monitor IC on the I²C bus — the primary BMS chip measuring cell voltages,
   charge current, and state of charge.

2. **Dual comm path** — The strings `"BL --> %s %s %d%d%d"` and
   `"AP --> %s %s %w"` suggest USART1 handles communication with both
   the bootloader ("BL") and application processor ("AP") on the
   main module. The `"I am VanMoof AP"` string confirms this firmware
   identifies as the "AP" side.

3. **Power modes** — Multiple power-on mode strings (UVP1, UVP2,
   OVP1, OVP2, "Shipping Mode", "MOS Failure Mode", "DP Mode",
   "VanMoof Mode") indicate the BMS manages a complex state machine
   for cell protection (under-voltage/over-voltage) and operational
   modes.

4. **Runtime-configurable vectors** — Three vector table slots point
   into SRAM (SysTick at `0x2000199C`, IRQ 25 at `0x20000E70`, IRQ 27
   at `0x20001AA0`). These are patched at runtime, likely by a
   bootloader hand-off or dynamic ISR registration API. This is
   characteristic of a firmware that accepts callbacks from different
   execution phases.
