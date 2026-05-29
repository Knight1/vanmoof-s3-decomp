# Memory map — batteryware

## MCU memory (STM32L072CZT6, ARM Cortex-M0+)

| Region | Address range | Size | Notes |
| --- | --- | --- | --- |
| Flash | `0x08000000` – `0x0802FFFF` | 192 KB | Boot from `0x00000000` aliases here at reset |
| SRAM  | `0x20000000` – `0x20004FFF` |  20 KB | |
| Peripherals | `0x40000000` – `0x5FFFFFFF` | — | APB1 / APB2 / AHB |
| Cortex-M0+ SCS | `0xE000E000` – `0xE000EFFF` | — | NVIC, SysTick, SCB |

Datasheet: *STM32L072xx Reference Manual (RM0376)* — see `reference/`.

## Firmware layout

The batteryware image (`batteryware_1.17.1.bin`, `0x15610` = 87,568 bytes)
is linked at **`0x08000000`** (STM32L0 flash base). Unlike shifterware,
there is no separate bootloader occupying the lower flash — batteryware is
self-contained at the flash origin.

> Image base confirmed 2026-05-25 by setting `image_base = 0x08000000`
> in Ghidra; the reset vector `0x080131F9` (Thumb LSB set → entry at
> `0x080131F8`) only lands inside the image at this base.

### Image layout (link address `0x08000000`)

| Range | Size | Content |
| --- | --- | --- |
| `0x08000000`–`0x08000027` | 40 B | VanMoof image header |
| `0x08000028`–`0x080000E7` | 192 B | Cortex-M0+ vector table (48 × 4 B) |
| `0x080000E8`–`0x080155ED` | ≈87.3 KB | Application — `.text` / `.rodata` / `.data` |
| `0x080155EE`–`0x0801560F` | ≈34 B | Tail (possibly unused padding) |

### VanMoof image header (`0x08000000`–`0x08000027`)

| Offset | Bytes | Value (this build) | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | `0xAA55AA55` | Magic |
| `+0x04` | 4 | `0x011701B1` | Version word — packed `MAJOR.MINOR.PATCH.TYPE`: `MAJOR=0x01`, `MINOR=0x17` (=23), `PATCH=0x01`, `TYPE=0xB1` (batteryware type ID). Matches filename `1.17.1` |
| `+0x08` | 4 | `0x2E0150DA` | CRC32 — MPEG-2 polynomial, over image with this field and `length` blanked |
| `+0x0C` | 4 | `0x00015610` | `imageSize` — bytes from `0x08000000` to end (87,568) |
| `+0x10` | 12 | build date (ASCII, null-terminated) | |
| `+0x1C` | 9 | build time (ASCII, null-terminated) | |
| `+0x24` | 4 | `0xFFFFFF00` | Padding / reserved |

### Cortex-M0+ vector table (`0x08000028`–`0x080000E7`)

Standard Cortex-M0+ layout, 48 entries (16 system + 32 IRQ). Addresses
shown with LSB set (Thumb mode).

| Index | Offset | Value | Meaning |
| --- | --- | --- | --- |
| 0 | `+0x00` | `0x20005000` | Initial MSP. Stack grows down from top of 20 KB SRAM |
| 1 | `+0x04` | `0x080131F9` | **Reset_Handler** (entry at `0x080131F8`) |
| 2 | `+0x08` | `0x0801324D` | NMI_Handler (default) |
| 3 | `+0x0C` | `0x0800B329` | **HardFault** → real handler `0x08006328` (`system_reset_simple`, value − 0x5000). The `0x0800B328` label is a −0x5000 mislabel (lands mid-UART-processor). |
| 4–10 | — | `0x00000000` | MemManage, BusFault, UsageFault, Reserved (all zero — CM0+ faults handled via HardFault) |
| 11 | `+0x2C` | `0x0801324D` | SVC_Handler (default) |
| 12–13 | — | `0x00000000` | Reserved |
| 14 | `+0x38` | `0x0801324D` | PendSV_Handler (default) |
| 15 | `+0x3C` | `0x2000199D` | **SysTick_Handler** — → SRAM vector at `0x2000199C` (configurable at runtime) |
| 16–27 | — | `0x0801324D` | IRQ 0–11 (most default, except IRQ 5 = `0x0800C24D` → real `0x0800724C` EXTI0_1/PB0, IRQ 7 = `0x0800C279` → real `0x08007278` EXTI4_15/PC13, IRQ 12 = `0x080054C5` → real `0x080004C4` ADC1_COMP — all values − 0x5000) |
| 28 | `+0x70` | `0x080054C5` | **ADC1_COMP_IRQHandler** (IRQ 12) — real handler at `0x080004C4` (value − 0x5000; the `0x080054C4` label is a mislabel) |
| 29–34 | — | `0x0801324D` | IRQ 13–18 (default) |
| 35 | `+0x8C` | `0x00000000` | IRQ 19 — zero |
| 36–40 | — | `0x0801324D` | IRQ 20–24 (default) |
| 41 | `+0xA4` | `0x20000E71` | IRQ 25 — → SRAM vector at `0x20000E70` |
| 42 | `+0xA8` | `0x0801324D` | IRQ 26 (default) |
| 43 | `+0xAC` | `0x20001AA1` | IRQ 27 — → SRAM vector at `0x20001AA0` |
| 44–45 | — | `0x0801324D` | IRQ 28–29 (default) |
| 46 | `+0xB8` | `0x00000000` | IRQ 31 — zero |
| 47 | `+0xBC` | `0x0801324D` | Vector table end (default) |

The default handler `0x0801324D` (Thumb LSB set → entry at `0x0801324C`)
is an infinite loop stub (`b .`).

Real IRQ handlers identified so far (vector values are runtime addresses;
subtract 0x5000 to get the Ghidra/file address of the real handler):
- **Slot 3 (HardFault)** → `0x08006328` = `system_reset_simple` (OEM resets on fault). The `0x0800B328` blob is the +0x5000 mislabel (mid-UART-processor).
- **IRQ 5 (EXTI0_1)** → `0x0800724C` — clears EXTI line 0 = PB0 button. (Earlier "TIM2 at 0x0800C24C" was the mislabel.)
- **IRQ 7 (EXTI4_15)** → `0x08007278` — clears EXTI line 13 = PC13 power button. (Earlier "TIM2 at 0x0800C278" was the mislabel.)
- **IRQ 12 (ADC1_COMP)** → `0x080004C4` — ADC EOC/OVR ISR (sample buffer @ `0x20002558`). (Earlier "USART1 at 0x080054C4" was the mislabel.)
- SysTick at SRAM `0x2000199C` — configurable at runtime via `s_sys_tick_callback`
- IRQ 25 at SRAM `0x20000E70` — runtime-configurable vector
- IRQ 27 at SRAM `0x20001AA0` — runtime-configurable vector

## SRAM globals

| Address | Size | Name | Notes |
| --- | --- | --- | --- |
| `0x20000E70` | 4 | — | Runtime IRQ vector (IRQ 25 target) |
| `0x2000199C` | 4 | `s_sys_tick_callback` | Runtime SysTick callback pointer |
| `0x20001AA0` | 4 | — | Runtime IRQ vector (IRQ 27 target) |

## Peripheral base addresses (STM32L0)

| Peripheral | Base | Notes |
| --- | --- | --- |
| GPIOA | `0x50000000` | STM32L0 GPIO |
| GPIOB | `0x50000400` | |
| GPIOC | `0x50000800` | |
| RCC | `0x40021000` | Reset & Clock Control |
| USART1 | `0x40013800` | Serial comm with main module |
| TIM2 | `0x40000000` | General-purpose timer |
| IWDG | `0x40003000` | Independent watchdog |
| ADC | `0x40012400` | Analog-to-digital converter |
|FEDL5236 (communicates via SPI)1 | `0x40005400` | I²C bus 1 |
| Flash | `0x40022000` | Flash controller |

## Key observations

1. **CM0+ M0+** — STM32L072 is Cortex-M0+, not M0. Supports privileged/unprivileged mode and has a real VTOR register (`SCB_VTOR` at `0xE000ED08`), unlike Cortex-M0. This means the bootloader could remap the vector table at runtime.

2. **FEDL5236** — References in the strings point to the **FEDL5236** fuel gauge / battery monitor IC (likely Fortior Tech). This is the primary BMS chip communicating over I²C.

3. **Dual USART?** — Strings reference both "BL →" and "AP →" messages, suggesting USART1 handles communication with both the bootloader and the application processor on the main module.

4. **VanMoof AP** — The `"I am VanMoof AP"` string confirms this firmware runs on the "application processor" side of the battery BMS.
