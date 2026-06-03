# powerbankboot — hardware

STM32F091xC (LQFP, Cortex-M0). Same silicon as `powerbankware`; the loader uses
only a small slice of it. All addresses below are read out of
`powerbank_bootloader_1.00.bin`.

## Peripheral bases used by the loader

| Peripheral | Base | Role in the loader |
| --- | --- | --- |
| RCC | `0x40021000` | clock tree, peripheral clock enables, reset-flag clear |
| FLASH (reg) | `0x40022000` | `ACR` prefetch enable; erase/program via HAL |
| RTC | `0x40002800` | **backup registers** hold the persisted upgrade flag |
| IWDG | `0x40003000` | independent watchdog (reload key `0xAAAA`) |
| USART1 | `0x40013800` | debug-trace console (`dbg_printf`) |
| USART2 | `0x40004400` | **comms / OTA bus** (interrupt-driven, IRQ28) |
| GPIOA… | `0x48000000` | port clocks IOPA/B/C/F enabled in `gpio_init` |

### Clocks (`clock_periph_init` @ `0x08001C08`)

HAL bring-up: `HAL_RCC_OscConfig` → `HAL_RCC_ClockConfig` (flash latency 1) →
`HAL_RCCEx_PeriphCLKConfig` (RTC + USART). Before configuring, it sets
`RCC_APB1ENR.PWREN` and the `RCC_BDCR` LSE-drive bits, and afterwards
`RCC_BDCR.RTCEN`. The RTC runs from LSE (32.768 kHz; async/sync prediv
`0x7F`/`0xFF`). USART1 is initialised here as the debug console; the comms USART2
is brought up separately (`comms_uart_hw_init`, `FUN_080033F4`, in the
`boot_hw_init` chain) and is the one wired to the NVIC (IRQ28).

### Watchdog (`iwdg_init` @ `0x08001E4C`)

IWDG: prescaler `/64`, reload `0x4E2` (1250 → ~2 s on the 40 kHz LSI), window
disabled. Refreshed by writing `0xAAAA` to `IWDG_KR` — this is the recurring
`0xAAAA` seen in the server loop (every ~250 ticks) and in `goto_application()`
just before the jump. The OTA "busy" mode bit suppresses the kick while a flash
transfer is in flight.

### USART (STM32F0 register layout, confirmed)

`CR1 @ +0x00`, `ISR @ +0x1C`, `ICR @ +0x20`, `RDR @ +0x24`, `TDR @ +0x28`.
`USART2_IRQHandler` (`0x08002248`) is a hand-rolled HAL-style ISR over two ring
buffers: RXNE → RX ring, TXE → TX ring, TC → mark line idle, errors cleared via
`ICR`. The driver state bytes mirror the HAL UART handle (`gState @ +0x69`,
`RxState @ +0x6A`, value `0x20` = `HAL_UART_STATE_READY` = `' '`).

### Timers

Two non-default IRQ vectors beyond SysTick:
- **IRQ17 / TIM6_DAC** (`0x08001158`) — STL clock cross-measurement input
  capture (the "Xmeas" / "LSE = %ld" / "HSE = %ld" Class-B clock check).
- **SysTick** (`0x0800214C`) — millisecond tick; posts sub-rate event bits to
  the download server loop (see `protocol.md`).

## Persisted state — RTC backup registers

`rtc_bkp_read/write` address `RTC_BASE + 0x50 + idx*4` (`RTC_BKPxR`). The loader
keeps the "upgrade finished" flag redundantly:

| Reg | Index | Contents |
| --- | --- | --- |
| `RTC_BKP0R` | 0 | `fBMS_Upgrade_Finish` value |
| `RTC_BKP1R` | 1 | its bitwise complement (validity guard) |

On boot, `boot_read_persistent_flags` loads both and zeroes the flag if
`BKP0 != ~BKP1`. `store_boot_flag(v)` writes the pair; `goto_application()` and
the server-loop entry call `store_boot_flag(0)` to disarm a consumed upgrade.

## Known SRAM globals (OEM addresses)

Reconstructed from the literal pools; the C decomp uses named globals rather than
pinning these addresses (behaviour-equivalent, not yet byte-placed).

| Address | Symbol (C) | Meaning |
| --- | --- | --- |
| `0x20000BB4` | `g_upgrade_finished` | upgrade-finished flag (mirrors `BKP0R`) |
| `0x20000BBC` | `g_boot_events` | SysTick → server-loop event bits |
| `0x20000B50` | `g_loop_mode` | bit0 = transfer busy, bit1 = upgrade finalised |
| `0x20000B54` | IWDG handle | holds `IWDG_BASE`; `*handle = 0xAAAA` refreshes |
| `0x20000B94` | RTC handle | `g_hrtc_obj` |
| `0x20000184` | USART1 handle | debug console |
| `0x20001FCC` | USART2 handle | comms bus (drained/pumped by the loop + ISR) |
| `0x20000BC4` | RX ring (1024 B) | USART2 receive ring; wrap limit `0x3FF` |
| `0x20000FCC` | TX ring (4096 B) | USART2 transmit ring; wrap limit `0xFFF` (ends exactly at the USART2 handle) |
| `0x20000B42` | `s_ota.state` | download protocol state (0/1/2) |
| `0x20000B40` | `s_ota.cmd` | current command byte (`0x31` / `0x21`) |
| `0x20000A38` | `s_ota.idx` | byte index within the current phase |
| `0x20000A3C` | `s_ota.arg[5]` | 4-byte value + XOR check buffer |
| `0x20000B64` | `g_hcrc` | HAL CRC handle (image verify) |

## Vendor-stock blocks (not decoded)

- **ST X-CUBE-STL** (IEC-60730 Class-B self-test): CPU/RAM/Flash-CRC/clock tests,
  the control-flow signature counters, `FailSafe`, the reset-cause logger and the
  clock cross-measurement. Roughly the `0x08000940..0x08000F00`,
  `0x08001100..0x08001200` and `0x08002A00..0x08002C00` clusters.
- **STM32F0 HAL** — RCC / FLASH / RTC / UART / GPIO / IWDG (`0x08003000..0x08004F00`).
- **tinyprintf** formatter behind `dbg_printf` (`0x08005A00..0x08006900`); the
  `"0123456789abcdef"` / `"#-0+ "` tables at `0x08006D44+` are its digit/flag sets.
