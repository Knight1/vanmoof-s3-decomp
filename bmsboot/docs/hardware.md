# bmsboot — hardware

## Identity

`bmsboot_v007.bin` is the **VanMoof battery-module (BMS) bootloader** — the 20 KB
loader that owns the base of flash on the in-frame battery's STM32L072CZT6 and
validates / installs / launches the [`../batteryware`](../batteryware)
application. The banner string is `"\nI am VanMoof BL V007 2022-11-04 09:32:30\r"`.

It is the same loader family ("VanMoof BL") as the STM32F091 PowerBank loader
[`../powerbankboot`](../powerbankboot): the same A/B-bank + "WHO?" serial-download
design, recompiled for the STM32L0. Decomp discipline: C here is derived from
**this** binary's own disassembly; powerbankboot is confirmation only for the
shared loader core.

## MCU

| | |
| --- | --- |
| Part | STM32L072CZT6 (LQFP-64) |
| Core | ARM Cortex-M0+, Thumb, 32 MHz max, **VTOR present** |
| Flash | `0x08000000` – `0x0802FFFF` (192 KB) |
| EEPROM | `0x08080000` – `0x080817FF` (6 KB, byte-writable) |
| SRAM | `0x20000000` – `0x20004FFF` (20 KB) |
| Loader image | 20 KB (`0x08000000`–`0x08005000`) |

## Peripheral bases (used by the loader)

| Peripheral | Base | Notes |
| --- | --- | --- |
| RCC | `0x40021000` | IOPENR `+0x2C`, AHBENR `+0x30` (CRCEN bit12), APB2ENR `+0x34` (USART1EN bit14), APB1ENR `+0x38` (PWREN bit28), CSR `+0x50` (reset flags, RMVF bit23) |
| PWR | `0x40007000` | CR: VOS range 1 (`(x & 0xFFFFE7FF) \| 0x800`) |
| FLASH | `0x40022000` | ACR `+0x00` (PRFTEN), PECR `+0x04`, PEKEYR `+0x0C`, PRGKEYR `+0x10` |
| IWDG | `0x40003000` | KR reload key `0xAAAA`; HAL_IWDG_Init prescaler /16, reload `0x908` |
| USART1 | `0x40013800` | 9600 8N1, PA9/PA10 (AF4); CR1 `+0x00`, CR3 `+0x08`, ISR `+0x1C`, ICR `+0x20`, RDR `+0x24`, TDR `+0x28` |
| CRC | `0x40023000` | HW CRC-32 (MPEG-2) used by `image_verify`; CR `+0x08` RESET bit |
| GPIOA/B/C/H | `0x50000000` / `0x50000400` / `0x50000800` / `0x50001C00` | |
| SCB | `0xE000ED00` | VTOR `+0x08` (set to `0x20000000`), AIRCR `+0x0C` (`0x05FA0004` system reset) |
| SysTick | core | 1 ms tick (`HAL_InitTick`), priority 3 |

## GPIO (from `gpio_init` / `led_init` / `download_pin_check`)

The full board setup configures pins across GPIOA/B/C/H. Identified roles:

| Pin(s) | Role |
| --- | --- |
| PA9 / PA10 | USART1 TX / RX (AF4) — the comms / OTA bus |
| **PA10** | also read at boot by `download_pin_check`: high → start the serial-download server |
| GPIOB `0x0001/0x0002/0x0004/0x0200` (PB0/PB1/PB2/PB9) | status LEDs (`led_init` drives them low) + PB0 lit during a Shadow→AP install |

Remaining GPIOA/B/C/H pin masks (`0x91CF`, `0x8287`, `0x7104`, `0x911F`, `0xF387`,
`0x0E00`, `0x0C40`, GPIOC `0x2000`, GPIOH `0x0001`/`0x0002`) are board enable /
sense lines configured by `gpio_init`; their individual functions are not yet
resolved (the HAL `GPIO_Init` leaf is vendor-stock).

## SRAM globals (named; not byte-placed in the rebuild)

| Symbol | OEM addr | Meaning |
| --- | --- | --- |
| `g_boot_events` | `0x200008BC` | SysTick → super-loop event bits (bit0..6) |
| `g_boot_countdown` | `0x2000088C` | boot-delay countdown (re-armed to `0xC8`) |
| `g_loop_flags` | `0x20000850` | bit0 busy (download in progress), bit1 upgrade-finished, bit2 recovered |
| `g_systick_ms` | `0x20001D50` | free-running millisecond counter |
| `s_reset_flags` | `0x20000888` | saved `RCC_CSR` (persisted to EEPROM `0x08080002`) |
| OTA state | `0x200005E8`..`0x200007F6` | `ota_process_byte` machine (state/idx/cmd/addr/acc, frame+data buffers) |
| TX / RX rings | `0x200008C0` / `0x200018C0` | 4096 B / 1024 B |
| USART1 handle | `0x20001CC0` | HAL `UART_HandleTypeDef` (gState `+0x78`, RxState `+0x7C`, ErrorCode `+0x80`) |
| CRC handle | `0x20000864` | HAL `CRC_HandleTypeDef` |

## Watchdog

The IWDG is armed in `boot_hw_init` (`iwdg_hal_init`, prescaler /16, reload
`0x908`) and kicked (`IWDG_KR = 0xAAAA`) from the super-loop (event bit6, while no
transfer is busy), around every flash op during a copy/download, and once more in
`goto_application()` just before the jump. A persistent flash erase/program or HAL
init failure trips `failsafe()`, which issues a `system_reset()` (SCB AIRCR).
