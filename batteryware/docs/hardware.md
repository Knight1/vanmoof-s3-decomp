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
| PA4 (bit `0x10`) | Output | **Status "alive" pulse** (we previously called this the "LED driver"). Pulsed by `led_flash` at `0x0800527C` via `gpio_bit_write(GPIOA, 0x10, 1/0)` — high 100 ms / low 50 ms, or 20 ms / 10 ms when the `g_pFastModeLed` flag (`0x20002BFC`) is set. **Called only from `bms_init` at boot.** No `LED`/`fuse` string exists in the image, so "LED" is a decomp guess; on a board with no populated LED this is likely a DNP footprint or an off-board status line. It is **not** the fuse — a heater driven every boot would blow the pack. | `led_flash` @ `0x0800527C` ← `bms_init` |
| PB7 (bit `0x80`) | Output | **Secondary-protection fuse heater (very likely).** The *only* GPIOB output that is driven HIGH and **never** driven LOW at runtime (no `gpio_bit_write(…, 0x80, 0)` exists; cleared once at boot in the `0x0287` init group). Asserted solely in `state_handler_17_19` when a hard over-current is latched (`g_fault_flags` bit 6 `FAULT_DISCHARGE_OC` or bit 7 `FAULT_CHARGE_OC`, with `s_prot_status` bit 11 clear and `s_bms_cfg` bit 15 clear), at the same moment it force-opens both FETs (`charge_mosfet_off`, `bms_configure(0)`). One-shot, fault-gated, never reset = textbook pyro/thermal-fuse trigger (permanent pack disconnect on "MOS Failure"). Verify: PB7 → small FET/transistor → resistive heater leg of the fuse. | `state_handler_17_19` @ `0x080054cc` |
| PB1 (bit `0x2`) | Output | Charge-path control, toggled both ways by `charge_mosfet_on` / `charge_mosfet_off` (charge.c) and pulsed in `state_handler_17_19` / fault paths. (`fault_led_trigger` also drives it — see note below.) | charge.c, state_handlers.c |
| PB9 (bit `0x200`) | Output | **Charge MOSFET enable.** `charge_mosfet_set` drives `gpio_bit_write(GPIOB, 0x200, on)` and caches state in `s_bms_cfg` bit 6. Matches the `"\nChargeOn_Off() --> PB9=0\r"` string. | `charge_mosfet_set` @ `0x08002d50` |
| PB12 (bit `0x1000`), PB15 (bit `0x8000`) | Output | Misc control lines (toggled both ways) — PB12 cleared in modem.c / `state_flags_handler_timer`, PB15 set in `bms_setup` / `cell_balance_update`. Roles not yet pinned down. | bms_setup.c, fuel_gauge.c, modem.c |
| PB11 (bit `0x800`) | Input | **Mode-select button.** `main` reads it via `gpio_bit_read(GPIOB, 0x800)` at boot: pressed → "VanMoof Mode" + `s_bms_cfg |= 8`; released → "DP Mode" + `s_bms_cfg &= ~8`. | `main` @ `0x080057b0` |
| PA9 / PA10 (mask `0x600`) | AF4 (USART1 TX/RX) | **Service/debug UART.** Normally idle (PA10 is sampled as an input). When PA10 is read high for >9 consecutive ticks in `state_timer_05`, `service_uart_init` reconfigures PA9/PA10 to AF4 and brings up USART1 @ 9600 baud, then sets the TX-enable flag (`0x2000453D`). A jumper/probe on PA10 is the trigger. | `service_uart_init` @ `0x0800ab7c` ← `state_timer_05` |

> **Charge/discharge FETs are mostly in the FEDL5236 AFE, not on MCU GPIO.**
> `bms_configure` (`0x080052d8`) enables/disables the pack FETs by writing
> FEDL5236 registers 3–9 over SMBus; `discharge_mosfet_set` works purely
> through that config byte. PB9 (and PB1) are secondary MCU-side charge cuts
> layered on top of the AFE's protection FETs.
>
> GPIOB output reset state (from `gpio.c` init): `gpio_bit_write(GPIOB,
> 0x0287, 0)` drives PB0/1/2/7/9 LOW, then `gpio_bit_write(GPIOB, 0xF104, 1)`
> drives PB2/8/12/13/14/15 HIGH.

## SRAM globals

Addresses resolved from literal pool entries in the flash image.

| Address | Name (Ghidra) | Size | Description |
| --- | --- | --- | --- |
| `0x20000E70` | — | 4 | Runtime IRQ vector target (IRQ 25 → loaded from VT slot 41) |
| `0x2000199C` | — | 4 | Runtime SysTick callback pointer (VT slot 15 → `0x2000199D`) |
| `0x20001AA0` | — | 4 | Runtime IRQ vector target (IRQ 27 → loaded from VT slot 43) |
| `0x20002588` | — | — | Cell-status byte array. `[1]`/`[2]` are the two pack cell readings compared in `bms_state_machine` against the `cfg_blk` window thresholds; `[0]`/`[1]`/`[2]` are also copied into the telemetry packet by `bms_set_state`. |
| `0x200027FA` | — | 2 | OVP comparison threshold read by `main`'s boot mode-report (vs `cfg_blk[0x2e/0x32/0x36]`). |
| `0x20002800` | — | 4 | Pre-charge upper-window threshold (`thr2`) compared against `cfg_blk[0x16]` in the `bms_state_machine` precharge SM. |
| `0x2000282A` | — | 2 | UVP comparison threshold read by `main`'s boot mode-report (vs `cfg_blk[0x3a/0x3e/0x42/0x46]`). |
| `0x20002C48` | `g_boot_mode` | 1 | Power-on mode latched by `main` from external flash `0x08080001`; selects the boot UVP/OVP/MOS-failure report path. |
| `0x20002C70` | — | 2 | Cleared to 0 at the top of `main` (role TBD). |
| `0x20002820` | — | 2 | FEDL5236 status word — written byte-wise from the reg-10 read in `bms_set_state`, mirrored into the telemetry packet (`+0x2e`). |
| `0x20002870` | `mode_flag` | 1 | Idle BMS config selector. When not discharging hard, set to 1 (cfg bit12 clear) or 3 and passed to `bms_configure`; bit 1 gates whether the idle-relax branch runs. |
| `0x200028A0` | — | 4 | Pre-charge lower-window threshold (`thr1`) compared against `cfg_blk[0x16]`. |
| `0x200028D0` | `cfg_blk` | — | BMS configuration block. Byte/halfword fields: `+0x05` "ready" flag (set by `bms_set_state`), `+0x16` upper-window voltage (u16), `+0x6e/0x72/0x76/0x7a` cell-voltage trip/recover thresholds (u8), `+0x70/0x74/0x78/0x7c` debounce counts (u16, ÷100). |
| `0x200029A8` | `bms_ctx` | 0x40 | **BMS telemetry/EEPROM context.** Per-state transition counters at `+0x04..+0x22` (u16), housekeeping words at `+0x24/0x28/0x2c/0x34/0x36/0x37`, and the rolling sequence counter at `+0x3e` (u16, wraps at 64999). Persisted field-by-field to ext-flash 0x08080C00+ by `bms_set_state`. |
| `0x20002AD0` | — | 0x38 | Telemetry record scratch buffer assembled by `bms_set_state` before it is written to the two 50-entry ext-flash ring buffers (0x08080200 / 0x08080E00). |
| `0x20002B58` | `s_state` | 1 | **Live BMS state byte.** Set by `bms_set_state` to the requested state (previous saved to 0x2000299C); `bms_state_machine` then force-writes 3 after each protection transition. |
| `0x20002BFE` | `pre_state` | 1 | Pre-charge/MOSFET-balance sub-state (signed: -1 idle/abort, 1/2 ramp, 3/4 hold). |
| `0x20002C06` | `recover_cnt` | 2 | OVP-recovery hold counter (clears the cfg-bit7 charge-disable latch after 50 ticks). |
| `0x20002C46` | `pre_cnt` | 2 | Pre-charge state-machine debounce counter. |
| `0x2000286C` | `s_prot_status` | 2 | OVP/UVP **protection-status** word. Read by the BMS state timers (`ldrh`) to dispatch protection handlers (bits 0→`07`, 1→`08`, 5→`01`, 11→`17_19`); zeroed by `bms_init`. Not the central fault register. |
| `0x2000289C` | `s_bms_state` | 1 | BMS dispatch byte — when non-zero, the state timers jump to `state_handler_11`. (`s_status_lo` in `bms_init.c`.) |
| `0x200028C8` | `s_bms_cur` | 4 | Over-current measurement cell, compared against `19999` (`0x4E1F`) in `state_handler_0d`/`0e`. |
| `0x20002BFC` | — | 1 | `g_pFastModeLed` target — boolean flag controlling the PA4 status-pulse speed. `0` = slow (100ms/50ms), non-zero = fast (20ms/10ms). Read by `led_flash`. |
| `0x20002C00` | `s_bms_cfg` | 4 | **Central BMS status/control register.** Bit-flag word driving the state machine (bit 4 clear-on-entry, bit 11 mask, bits 3/11/12 select `bms_configure` arg). Aliased as `s_status`/`g_state_timer`/`s_mosfet_status` across files — all the same cell. |
| `0x20002C10` | — | 4 | SysTick reload value holder — written by `delay_ms` ISR path. Initial value `0x0000AAAA`. |
| `0x20002C44` | `s_fault_flags` / `g_fault_flags` | 4 | **Central fault/protection register** (`g_fault_flags` in `fuel_gauge.c`/`batteryware.h`). State timers read its low half (`ldrh`) — bits 5/6/7 dispatch `state_handler_17_19`. |
| `0x20004488` | `s_uart_base` | 0x84 | **USART1 service-UART handle** (HAL_UART_HandleTypeDef-shaped). `[0]`=instance (`0x40013800`), `[1]`=baud (9600), `[5]`=mode (TX\|RX), `[9]/[0xF]`=advanced-feature words; rx/tx bookkeeping at `+0x78/0x7C/0x80`. Populated by `service_uart_init`, shared with `uart.c`. |
| `0x2000453D` | `s_tx_enabled` | 1 | **Service-UART TX-enable flag.** Set 1 by `service_uart_init` when PA10 held (UART up), cleared 0 otherwise; gates UART TX in `uart.c`. (Same cell `state_timer_05` calls `s_gpio_flag`.) |
| `0x2000453E` / `0x20004540` / `0x20004542` / `0x20004544` | — | 2 ea | USART1 RX/TX ring-buffer index cells, zeroed by `service_uart_init` at bring-up. |
| `0x20002C84` | — | 2 | Zeroed by `service_uart_init` at UART bring-up (role TBD). |
| `0x20002C80` | `s_bms_flags` | 1/4 | **BMS dispatch-flags byte** — read by the state timers as `uint8` (`ldrb`), bit 0 / bit 2 gate cell-balance and fault-dispatch, cleared after handling. NOTE: `delay.c`/this table previously called it a "SysTick CTRL shadow" (`uint32`) — that interpretation is unverified and likely wrong (SysTick CTRL is a core register at `0xE000E010`, not SRAM). |

## Peripheral usage

| Peripheral | Base | Confirmed usage |
| --- | --- | --- |
| GPIOA | `0x50000000` | Status "alive" pulse on PA4 (BSRR/BRR via `gpio_bit_write`) — see GPIO table |
| GPIOB | `0x50000400` | PB9 = charge MOSFET enable (`"\nChargeOn_Off() --> PB9=0\r"`); **PB7 = secondary-protection fuse heater** (one-shot, OC-gated); PB1 charge-path control; PB12/PB15 misc |
| USART1 | `0x40013800` | Serial comm with main module (IRQ 12 = USART1 at VT slot 28 → `0x080054C5`); also the PA9/PA10 PA10-gated service UART (`service_uart_init`) |
| USART2 | `0x40004400` | Handled by `UART_SetConfig` (standard OVER8/OVER16 BRR); clock source from RCC->CCIPR |
| LPUART1 | `0x40004800` | Low-power UART — `UART_SetConfig` gives it the LPUART BRR path (`256×freq/baud`, range `[0x300,0xFFFFF]`); the prior decomp called this base "USART3" |
| TIM2 | `0x40000000` | IRQ 5 and 7 assigned (two TIM2 IRQs) |
| SysTick | `0xE000E010` | 1 ms tick timer — polled by `delay_ms` |
|FEDL5236 (communicates via SPI)1 | `0x40005400` | Likely FEDL5236 fuel gauge communication |

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
