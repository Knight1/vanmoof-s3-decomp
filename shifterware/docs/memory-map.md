# Memory map

## MCU memory (MM32F031F6U6, ARM Cortex-M0)

| Region | Address range | Size | Notes |
| --- | --- | --- | --- |
| Flash | `0x08000000` – `0x08007FFF` | 32 KB | Boot from `0x00000000` aliases here at reset |
| SRAM  | `0x20000000` – `0x20000FFF` |  4 KB | bit-band not present on M0 |
| Peripherals | `0x40000000` – `0x4FFFFFFF` | — | APB1 / APB2 / AHB1 / AHB2 |
| Cortex-M0 SCS | `0xE000E000` – `0xE000EFFF` | — | NVIC, SysTick, SCB |

Datasheet: *MM32F031xx Datasheet (V1.10 or later)* — see `reference/`.

## Firmware partition layout (confirmed)

The OEM image carries both **shifterboot** (loader) and **shifterware**
(application). The application portion (`shifterware_0.237.bin`,
`0x2EA8` bytes) is loaded in Ghidra at **link base `0x08003000`**
(confirmed 2026-05-11 from `shifter_program.json` — the reset vector
`0x08005E79` only lands inside the image when based at `0x08003000`).
The boot loader image (see `Shifterboot` line in
`../GolandProjects/VanMooof-Module/FIRMWARE.md`) is separate and presumed
to occupy `0x08000000`–`0x08002FFF` (12 KB).

### Image layout (link address `0x08003000`)

| Range | Size | Content |
| --- | --- | --- |
| `0x08003000`–`0x08003027` | 40 B | VanMoof image header (see below) |
| `0x08003028`–`0x080030E7` | 192 B | Cortex-M0 vector table (48 × 4 B) |
| `0x080030E8`–`0x08005E77` | ≈11.6 KB | Application `.text` / `.rodata` |
| `0x08005E78`–`0x08005EAF` | 56 B | Startup file: `Reset_Handler` + `Default_Handler` cluster |

End of image is `0x08005EA7` (inclusive); total size `0x2EA8` matches
the header's `imageSize` field.

### VanMoof image header (`0x08003000`–`0x08003027`)

| Offset | Bytes | Value (this build) | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | `0xAA55AA55` | Magic |
| `+0x04` | 4 | `0x00ED02C1` | Version / build id (unverified — TBD) |
| `+0x08` | 4 | `0x1E8EB125` | CRC32 or similar (unverified — TBD) |
| `+0x0C` | 4 | `0x00002EA8` | `imageSize` — bytes from `0x08003000` to end |
| `+0x10` | 12 | `"Oct 23 2020\0"` | Build date (ASCII, null-terminated) |
| `+0x1C` | 9 | `"14:09:11\0"` | Build time (ASCII, null-terminated) |
| `+0x24` | 4 | `0xFFFFFF00` | Padding / reserved |

Cross-check with `pack_unpack` in `../GolandProjects/VanMooof-Module/`
to confirm which of the two unknown words is the CRC.

### Cortex-M0 vector table (`0x08003028`–`0x080030E7`)

Standard Cortex-M0 layout, 48 entries (16 system + 32 IRQ). Highlights:

| Index | Offset | Value | Meaning |
| --- | --- | --- | --- |
|  0 | `+0x00` | `0x20000400` | Initial MSP. Stack grows down from `0x20000400` (1 KB stack region). The remaining 3 KB of SRAM (`0x20000400`–`0x20000FFF`) is available for `.data` / `.bss`. |
|  1 | `+0x04` | `0x08005E79` | `Reset_Handler` (Thumb LSB set → entry at `0x08005E78`) |
|  2 | `+0x08` | `0x08005E9F` | `NMI_Handler` (→ default-handler cluster) |
|  3 | `+0x0C` | `0x08005EA1` | `HardFault_Handler` |
| 11 | `+0x2C` | `0x08005EA9` | `SVC_Handler` |
| 14 | `+0x38` | `0x08005EAD` | `PendSV_Handler` |
| 15 | `+0x3C` | `0x08005CF1` | `SysTick_Handler` — *real* handler at `0x08005CF0` (not the default-handler tail) |
| 16+ | … | mix of `0x08005EB1` (default) and a few real handlers | IRQs |

The handlers at `0x08005E9F..0x08005EAF` are a cluster of 2-byte
`b .` (infinite loop) stubs that vector-table slots alias into. Look
for the **real** IRQ handlers by spotting vector targets outside this
cluster — e.g. `SysTick` at `0x08005CF0` above.

### VTOR caveat

Cortex-M0 base does **not** have VTOR. Two consequences:

- The boot ROM aliases flash `0x08000000` to `0x00000000` at reset, so
  the **shifterboot** vector table is what the CPU sees at reset.
- Hand-off from shifterboot to shifterware is therefore a direct branch
  to `0x08005E79` (Reset_Handler), not a vector remap. The shifterware
  vector table at `0x08003028` is never used by the CPU directly — it is
  there for documentation / future-proofing or because the build tool
  always emits one. Interrupts cannot reach the shifterware handlers
  unless shifterboot copies the vectors into SRAM and runs from there
  (TBD — read the shifterboot image to confirm).

## Peripheral base addresses (MM32F031xx)

| Peripheral | Base | Bus |
| --- | --- | --- |
| TIM2  | 0x40000000 | APB1 |
| TIM3  | 0x40000400 | APB1 |
| TIM6  | 0x40001000 | APB1 |
| TIM14 | 0x40002000 | APB1 |
| RTC   | 0x40002800 | APB1 |
| WWDG  | 0x40002C00 | APB1 |
| IWDG  | 0x40003000 | APB1 |
| SPI2  | 0x40003800 | APB1 |
| USART2| 0x40004400 | APB1 |
| I2C1  | 0x40005400 | APB1 |
| I2C2  | 0x40005800 | APB1 |
| PWR   | 0x40007000 | APB1 |
| SYSCFG| 0x40010000 | APB2 |
| EXTI  | 0x40010400 | APB2 |
| ADC1  | 0x40012400 | APB2 |
| TIM1  | 0x40012C00 | APB2 |
| SPI1  | 0x40013000 | APB2 |
| USART1| 0x40013800 | APB2 |
| TIM15 | 0x40014000 | APB2 |
| TIM16 | 0x40014400 | APB2 |
| TIM17 | 0x40014800 | APB2 |
| DMA1  | 0x40020000 | AHB1 |
| RCC   | 0x40021000 | AHB1 |
| FLASH | 0x40022000 | AHB1 (flash interface) |
| CRC   | 0x40023000 | AHB1 |
| GPIOA | 0x48000000 | AHB2 |
| GPIOB | 0x48000400 | AHB2 |
| GPIOC | 0x48000800 | AHB2 |
| GPIOD | 0x48000C00 | AHB2 |
| GPIOF | 0x48001400 | AHB2 |

The shifter PCB likely exposes only a small subset of these. Tick them
off in `peripherals.md` as they are observed in the decompiled OEM code.
