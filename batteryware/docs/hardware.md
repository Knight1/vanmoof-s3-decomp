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
| PB7 (bit `0x80`) | Output | **Secondary-protection fuse heater (confirmed).** The *only* GPIOB output that is driven HIGH and **never** driven LOW at runtime (no `gpio_bit_write(…, 0x80, 0)` exists; cleared once at boot in the `0x0287` init group). Asserted solely in `state_handler_17_19` when a hard over-current is latched (`g_fault_flags` bit 6 `FAULT_DISCHARGE_OC` or bit 7 `FAULT_CHARGE_OC`, with `s_prot_status` bit 11 clear and `s_bms_cfg` bit 15 clear), at the same moment it force-opens both FETs (`charge_mosfet_off`, `bms_configure(0)`). One-shot, fault-gated, never reset = pyro/thermal-fuse trigger (permanent pack disconnect on "MOS Failure"). Drives the discrete heater chain `PB7 → Q1017 → Q1015 → Q1016 → SCF9550 heater`; the **same** fuse is fired autonomously, on over-voltage, by the secondary-protection ICs — see [Secondary protection & the pyro fuse](#secondary-protection--the-pyro-fuse). | `state_handler_17_19` @ `0x080054cc` |
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

## Secondary protection & the pyro fuse

The pack's irreversible disconnect is a **3-terminal fuse with an integral
heater** (**SCF9550** — `F1001`/`F1002` are the two series fuse links in the
pack positive line `VP → BATT_a`, `R1074` ≈ 40 Ω is the heater tapped at their
midpoint). The Part Name is SFK-4045 45AK10. Energizing the heater melts the link and **permanently** opens the
pack — there is no firmware or hardware reset.

The heater is switched by a discrete gate chain with **two fully independent
trigger inputs**, both pulling the same gate node (**JN2**):

**Heater drive** (fires when JN2 is pulled toward GND):

- `Q1016` `BSZ340N08NS3` — power N-MOSFET, sinks the heater current to GND → melts the fuse.
- `Q1015` `BSS84`/`BSR315P` — P-MOSFET gating Q1016: when its gate (node **JN2**) is pulled below `+BATT`, Q1015 conducts `+BATT → R1072 (47 k) → Q1016` gate.
- JN2 idles at `+BATT` via `R1071` (100 k) and is clamped by `D1021` (`BZT52C20`, 20 V).

**Trigger A — MCU firmware, over-current** (the path in
[`compare-1.14.1/fuse.md`](compare-1.14.1/fuse.md)):

- `PB7 → R1070 (100 R) / R1077 (10 k) → Q1017 (2N7002, R1078 100 k pulldown) → R1076 (330 k) → JN2`.
- `PB7` HIGH turns `Q1017` on, pulling JN2 low. Firmware asserts PB7 **only** on a
  latched discharge-/charge-over-current (the "MOS-failure" backstop) — it does
  **not** assert it on over-voltage.

**Trigger B — secondary over-voltage, autonomous hardware** (no MCU involvement):

- Two **`S-8215AAD-K8T2U`** ICs — **`U1005`** and **`U1006`** — are ABLIC
  secondary-protection cell-overcharge monitors. Each watches up to 5 series cells: **U1005 covers
  cells 1–5** (taps `−BATT … V5`), **U1006 covers cells 6–10** (taps `V5 … +BATT`),
  so the full **10S** pack is monitored. Each cell tap is RC-filtered (1 kΩ +
  150 nF) and the IC supply clamped by a 33 V zener (`D1035`/`D1037`).
- For the `-AAD` option the datasheet gives **overcharge detection = 4.350 V/cell**,
  **hysteresis −0.250 V** (release 4.100 V), **detection delay 2.0 s**, **CMOS,
  active-HIGH** `CO` output (pin 8).
- Each `CO → 100 k → 2N7002` (`Q1020` for U1005, `Q1021` for U1006) →
  `330 k`/`200 k` → **JN2** (OR-combined via `D1036`). If **any** monitored cell
  stays above 4.35 V for 2 s, that IC drives its 2N7002 on, pulling JN2 low →
  the fuse blows.

So **over-voltage does blow the fuse** — through this dedicated hardware layer,
independent of the FEDL5236 AFE and of the MCU. Protection hierarchy:

| Layer | Detector | Over-voltage action | Over-current action | Reversible? |
| --- | --- | --- | --- | --- |
| Primary | FEDL5236 AFE + MCU firmware | open charge FET → recoverable state (`state_handler_14`/`15`) | open FETs; blow fuse if persistent (`state_handler_17_19`) | OV: yes / OC-fuse: no |
| Secondary | `U1005`/`U1006` (`S-8215AAD`) | **blow fuse** @ 4.35 V/cell, 2 s | — | no |

This closes the open item from the firmware-only fuse audit: the **MCU blows the
fuse on over-current only**, but the pack still has a hardware
**over-voltage → fuse** path via U1005/U1006. Both feed the one SCF9550 heater.

## SRAM globals

Addresses resolved from literal pool entries in the flash image.

| Address | Name (Ghidra) | Size | Description |
| --- | --- | --- | --- |
| `0x20000E70` | — | 4 | Runtime IRQ vector target (IRQ 25 → loaded from VT slot 41) |
| `0x2000199C` | — | 4 | Runtime SysTick callback pointer (VT slot 15 → `0x2000199D`) |
| `0x20001AA0` | — | 4 | Runtime IRQ vector target (IRQ 27 → loaded from VT slot 43) |
| `0x200024F4` | `s_dma_ctx` (dma.c) | 0x5C | **HAL-style ADC handle.** `[0]`=ADC instance base (`0x40012400`); `+0x20` byte software-trigger gate; `+0x30` DMA-handle ptr; `+0x54` `State` (HAL_ADC_STATE_* bitset); `+0x58` `ErrorCode` (HAL_ADC_ERROR_* bitset). Read/written by `ADC1_COMP_IRQHandler`; the `dma.c`/`modem.c` "dma_ctx" name is the same cell. |
| `0x20002550` | `s_subcnt` (fuel_gauge.c) | 1+ | ADC sub-cycle / cell counter. Low byte indexes the sample buffer (`cell*4`) in `ADC1_COMP_IRQHandler`; `cell_balance_update` advances it per sequence. (Shared with the DMA path.) |
| `0x20002554` | `s_dma_enabled` (dma.c) | 1 | **ADC sequence-ready flag.** Bit 0 set by `ADC1_COMP_IRQHandler` on EOS; polled and cleared by `cell_balance_update`. (Not a DMA-enable bit — the dma.c name is a mislabel.) |
| `0x20002558` | `s_adc_buf` (fuel_gauge.c) | 0x1E | **ADC sample buffer**, u16 per cell/phase (5 cells × 3 phases). Filled by `ADC1_COMP_IRQHandler` (EOC → `DR & 0xFFF`); consumed by `cell_balance_update`. |
| `0x20002582` | `s_dma_counter` (dma.c) | 1 | Conversion-in-sequence index. Selects the per-cell slot (`+conv`) in the sample buffer and post-increments in `ADC1_COMP_IRQHandler`. (Shared with the DMA path.) |
| `0x200027D4` | `s_cell_voltage_table` (fuel_gauge.c) | u16[N] | **Per-cell voltage array** (N = cell_count). Averaged pairwise and rewritten by `fg_cell_balance`; also the live cell map built by `calculate_rsoc`. |
| `0x200027FE` | `s_balance_idx` / cached cell_count (fuel_gauge.c) | 1 | Cached cell count; overwritten with the last balanced cell index by `fg_cell_balance`. |
| `0x2000286E` | `s_balance_threshold` / mid-cell average (fuel_gauge.c) | 2 | Mid-cell average `(sum − max − min) >> 3`; `fg_cell_balance` only equalises a pair when its average is within ±0x31 of this. |
| `0x20002588` | — | — | Cell-status byte array. `[1]`/`[2]` are the two pack cell readings compared in `bms_state_machine` against the `cfg_blk` window thresholds; `[0]`/`[1]`/`[2]` are also copied into the telemetry packet by `bms_set_state`. |
| `0x200027FA` | — | 2 | OVP comparison threshold read by `main`'s boot mode-report (vs `cfg_blk[0x2e/0x32/0x36]`). |
| `0x20002800` | — | 4 | Pre-charge upper-window threshold (`thr2`) compared against `cfg_blk[0x16]` in the `bms_state_machine` precharge SM. |
| `0x2000282A` | — | 2 | UVP comparison threshold read by `main`'s boot mode-report (vs `cfg_blk[0x3a/0x3e/0x42/0x46]`). |
| `0x20002C48` | `g_boot_mode` | 1 | Power-on mode latched by `main` from external flash `0x08080001`; selects the boot UVP/OVP/MOS-failure report path. |
| `0x20002C70` | — | 2 | Cleared to 0 at the top of `main` (role TBD). |
| `0x20002820` | — | 2 | FEDL5236 status word — written byte-wise from the reg-10 read in `bms_set_state`, mirrored into the telemetry packet (`+0x2e`). |
| `0x20002870` | `mode_flag` | 1 | Idle BMS config selector. When not discharging hard, set to 1 (cfg bit12 clear) or 3 and passed to `bms_configure`; bit 1 gates whether the idle-relax branch runs. |
| `0x200028A0` | — | 4 | Pre-charge lower-window threshold (`thr1`) compared against `cfg_blk[0x16]`. |
| `0x200028D0` | `cfg_blk` (`s_ctx`) | 0xb8 | **BMS protection-threshold block** (distinct from `bms_ctx` @ `0x200029A8`). Built by `config_init` (`FUN_08007368`) from firmware defaults + a few EEPROM-validated fields. Holds the 5 cell-voltage window comparators (`+0x6e/0x76/0x7e/0x86/0x8e` trip, `+0x72/0x7a/0x82/0x8a/0x92` recover, u8; `+0x70/…` trip-delay & `+0x74/…` recover-delay, u16 ÷100), the 2 over-current thresholds (`+0x98` discharge, `+0x9a` charge, u16; shared delay `+0x96`), the precharge window (`+0x16`), and the boot OVP/UVP detect set (`+0x2a…+0x46`, u16 mV). **Full map + default values: [`protection-config.md`](protection-config.md).** |
| `0x200029A8` | `bms_ctx` | 0x40 | **BMS telemetry/EEPROM context.** Per-state transition counters at `+0x04..+0x22` (u16), housekeeping words at `+0x24/0x28/0x2c/0x34/0x36/0x37`, and the rolling sequence counter at `+0x3e` (u16, wraps at 64999). Persisted field-by-field to ext-flash 0x08080C00+ by `bms_set_state`. |
| `0x20002AD0` | — | 0x38 | Telemetry record scratch buffer assembled by `bms_set_state` before it is written to the two 50-entry ext-flash ring buffers (0x08080200 / 0x08080E00). |
| `0x20002B58` | `s_state` | 1 | **Live BMS state byte.** Set by `bms_set_state` to the requested state (previous saved to 0x2000299C); `bms_state_machine` then force-writes 3 after each protection transition. |
| `0x20002BFE` | `pre_state` | 1 | Pre-charge/MOSFET-balance sub-state (signed: -1 idle/abort, 1/2 ramp, 3/4 hold). |
| `0x20002C06` | `recover_cnt` | 2 | OVP-recovery hold counter (clears the cfg-bit7 charge-disable latch after 50 ticks). |
| `0x20002C46` | `pre_cnt` | 2 | Pre-charge state-machine debounce counter. |
| `0x2000286C` | `s_prot_status` | 2 | OVP/UVP **protection-status** word. Read by the BMS state timers (`ldrh`) to dispatch protection handlers (bits 0→`07`, 1→`08`, 5→`01`, 11→`17_19`); zeroed by `bms_init`. Not the central fault register. |
| `0x2000289C` | `s_bms_state` | 1 | BMS dispatch byte — when non-zero, the state timers jump to `state_handler_11`. (`s_status_lo` in `bms_init.c`.) |
| `0x200028C8` | `s_bms_cur` | 4 | Over-current measurement cell, compared against `19999` (`0x4E1F`) in `state_handler_0d`/`0e`. |
| `0x20002BFC` | — | 1 | Flag byte. Read by `led_flash` as `g_pFastModeLed` (PA4 status-pulse speed: `0`=slow 100/50ms, non-zero=fast 20/10ms). The button EXTI ISRs also set bits here: `EXTI0_1` (PB0) → bit 1, `EXTI4_15` (PC13 power) → bit 0. |
| `0x20002C00` | `s_bms_cfg` | 4 | **Central BMS status/control register.** Bit-flag word driving the state machine (bit 4 clear-on-entry, bit 11 mask, bits 3/11/12 select `bms_configure` arg). Aliased as `s_status`/`g_state_timer`/`s_mosfet_status` across files — all the same cell. |
| `0x20002C10` | — | 4 | SysTick reload value holder — written by `delay_ms` ISR path. Initial value `0x0000AAAA`. |
| `0x20002C44` | `s_fault_flags` / `g_fault_flags` | 4 | **Central fault/protection register** (`g_fault_flags` in `fuel_gauge.c`/`batteryware.h`). State timers read its low half (`ldrh`) — bits 5/6/7 dispatch `state_handler_17_19`. |
| `0x20004488` | `s_uart_base` | 0x84 | **USART1 service-UART handle** (HAL_UART_HandleTypeDef-shaped). `[0]`=instance (`0x40013800`), `[1]`=baud (9600), `[5]`=mode (TX\|RX), `[9]/[0xF]`=advanced-feature words; rx/tx bookkeeping at `+0x78/0x7C/0x80`. Populated by `service_uart_init`, shared with `uart.c`. |
| `0x2000453D` | `s_tx_enabled` | 1 | **Service-UART TX-enable flag.** Set 1 by `service_uart_init` when PA10 held (UART up), cleared 0 otherwise; gates UART TX in `uart.c`. (Same cell `state_timer_05` calls `s_gpio_flag`.) |
| `0x2000453E` / `0x20004540` / `0x20004542` / `0x20004544` | — | 2 ea | USART1 RX/TX ring-buffer index cells, zeroed by `service_uart_init` at bring-up. |
| `0x20002C84` | — | 2 | Zeroed by `service_uart_init` at UART bring-up (role TBD). |
| `0x20002C80` | `s_bms_flags` | 1/4 | **BMS dispatch-flags byte** — read by the state timers as `uint8` (`ldrb`), bit 0 / bit 2 gate cell-balance and fault-dispatch, cleared after handling. NOTE: `delay.c`/this table previously called it a "SysTick CTRL shadow" (`uint32`) — that interpretation is unverified and likely wrong (SysTick CTRL is a core register at `0xE000E010`, not SRAM). |
| `0x200047D2` | — | 2 | **UART command RX index / state word.** State machine cursor for the command processor; in the cmd-0x10 streaming path (`flash_stream_handler`) it doubles as the running byte count into the command buffer. Reset to 0 on frame completion or CRC failure. |
| `0x20004548` | — | 8/var | **UART command / stream buffer.** `[0]`=0xAA, `[1]`=command byte; for cmd-0x10 holds the streamed header (`[2:3]` command word BE, `[4:5]` arg/half-length BE, `[6]` payload length) followed by the payload and trailing CRC-16. |
| `0x2000474C` | — | 0x80 | **OAD page-assembly buffer.** `flash_stream_handler`'s cw==0x82 path copies payload bytes here, then DMA-programs/verifies a 0x80-byte flash page into the staging area. |
| `0x20004749` | — | 1 | OAD page byte index into `0x2000474C`; wraps each 0x80-byte page (sign bit = page full). |
| `0x200047D8` | — | 4 | **OAD received-byte counter.** Bumped per streamed payload byte; compared against `0x5000` (image-complete gate) by the command processor's 0x82 report arm and the cmd-0x80 firmware-update body. |
| `0x20004088` | — | 0x400 | **USART1 RX ring buffer.** Filled by the RX ISR; drained by `uart_resp_handler` (head `0x20004540` / tail `0x20004544`, wrap at 0x400). |
| `0x20004510` | — | 0x2c | **ASCII command line buffer** (index at `0x20002C84`). Accumulated by `uart_resp_handler` until CR, then handed to `command_parser`. |
| `0x20004648` | — | var | **Telemetry response buffer.** `[0]`=0xAA, `[1]`=3, `[2]`=count; `uart_protocol_handler`'s cmd-3 cascade appends fields, then a CRC-16, and transmits `0x200047D0` bytes. |
| `0x200047D0` | — | 2 | Response length (starts at 3, bumped by each appended field/CRC byte). |
| `0x200047D4` | — | 2 | **Report count / cascade gate** = `cmdbuf[4:5]*2`; non-zero enables every cmd-3 telemetry field. |
| `0x20002C72` | — | 1 | cmd-0x80 firmware-update retry counter (reset triggers `system_reset` after 0x32 fails). |
| `0x200047DC` | — | 1 | cmd-0x80 firmware-update watchdog-clear cell. |
| `0x200047E0` | `s_flash_mutex` | 0x18 | **Flash/EEPROM/SPI write mutex + error shadow.** `[+0x10]` (`0x200047F0`) = held flag (1=busy → callers return 2); `[+0x14]` (`0x200047F4`) = error-code shadow, zeroed on acquire and OR'd with a compressed FLASH_SR error code by `dma_error_clear`. Shared by `spi_register_write`, `flash_word_write`, `memcmp_verify`, `atomic_copy_16words`, `dma_channel_reset_all`. |

## External flash / EEPROM map

Addresses in `0x08080000..` are the external SPI flash / EEPROM region (accessed
via the SPI-poll helpers `memcpy_oem` for reads and `memcmp_verify` for
write+verify). `0x0801A800..` is the in-internal-flash OAD staging area.

| Address | Size | Description |
| --- | --- | --- |
| `0x08080001` | 1 | Power-on boot-mode selector (latched into `g_boot_mode`). |
| `0x0808000F..0x0808001F` | 9×2 | **Calibration pair table** written by `flash_stream_handler` (cmd-0x10, command word < 0x15). One 16-bit pair admitted per threshold step `cw<0x0d..0x15`. |
| `0x08080021 / 25 / 29` | 4 ea | **Anti-replay tick reference triplet** (`tick_val` / `tick_ms` / `tick_timeout`). Loaded for the differ-check, re-committed after the last calibration pair. |
| `0x08080200 / 0x08080E00` | 50×0x38 | Two telemetry ring buffers (`bms_set_state` writer; cmd-0xF45 history hex-dump reader). |
| `0x08080C00` | 0x80 | BMS config/telemetry context persisted by `bms_set_state`. |
| `0x08004FFC` | 4 | Expected-CRC word read by the cmd-0x80 firmware-update verify loop. |
| `0x0801A800` | 0x5000 | **OAD image staging area** in internal flash. Pages programmed `(page_addr & ~0x7F) + 0x0801A800` by `flash_stream_handler` (cw==0x82) and the cmd-0x80 body; `0x0801A7FC` holds the preceding descriptor word. |

## Peripheral usage

| Peripheral | Base | Confirmed usage |
| --- | --- | --- |
| GPIOA | `0x50000000` | Status "alive" pulse on PA4 (BSRR/BRR via `gpio_bit_write`) — see GPIO table |
| GPIOB | `0x50000400` | PB9 = charge MOSFET enable (`"\nChargeOn_Off() --> PB9=0\r"`); **PB7 = secondary-protection fuse heater** (one-shot, OC-gated); PB1 charge-path control; PB12/PB15 misc |
| USART1 | `0x40013800` | Serial comm with main module; also the PA9/PA10 PA10-gated service UART (`service_uart_init`). USART1 IRQ = IRQ27 (vector slot 43 → runtime-installed SRAM trampoline `0x20001AA0`) |
| EXTI | `0x40010400` | External-interrupt controller (PR @ +0x14). **IRQ5 (EXTI0_1)** clears line 0 = PB0 button; **IRQ7 (EXTI4_15)** clears line 13 = PC13 power button. Both enabled by `system_init`; ISRs set bits 0/1 @ `0x20002BFC`. Real vector targets `0x0800724C` / `0x08007278` (formerly mis-named `uart_check_*_error`) |
| USART2 | `0x40004400` | Handled by `UART_SetConfig` (standard OVER8/OVER16 BRR); clock source from RCC->CCIPR |
| LPUART1 | `0x40004800` | Low-power UART — `UART_SetConfig` gives it the LPUART BRR path (`256×freq/baud`, range `[0x300,0xFFFFF]`); the prior decomp called this base "USART3" |
| TIM2 | `0x40000000` | Timer base. (Earlier note "IRQ 5 and 7 = TIM2" was wrong: the IRQ5/IRQ7 `system_init` enables are EXTI0_1/EXTI4_15 buttons — see the EXTI row.) |
| ADC1 | `0x40012400` | 12-bit ADC for cell-voltage acquisition. **IRQ12 (ADC1_COMP, vector slot 28)** services it: EOC stores `DR & 0xFFF` into the sample buffer `@ 0x20002558`, EOS raises the sequence-ready flag `@ 0x20002554`, OVR records an error in the HAL handle `@ 0x200024F4`. Real vector target `0x080004C4` (now wired; was trapping via `Default_Handler`). Regs used: ISR +0x00, IER +0x04, CR +0x08, CFGR1 +0x0C, DR +0x40. |
| SysTick | `0xE000E010` | 1 ms tick timer — polled by `delay_ms` |
|FEDL5236 (communicates via SPI)1 | `0x40005400` | Likely FEDL5236 fuel gauge communication |
| FLASH/EEPROM ctrl | `0x40022000` | STM32L0 flash + data-EEPROM controller. **ACR** +0x00 (PRFTEN/LATENCY), **PECR** +0x04 (PROG=bit3, FPRG=bit10/0x400, ERASE=bit9/0x200), **SR** +0x18 (BSY=bit0, EOP=bit1, error bits 0x100/0x200/0x400/0x800/0x2000/0x10000/0x20000). All flash-program/erase + EEPROM-write paths poll SR via `dma_wait_for_ready`/`dma_wait_done` (mis-named "dma_*"; they are NOT DMA). Half-page program (`atomic_copy_16words`) = FPRG+PROG then 16 words to a fixed PECR latch. Page erase = `dma_channel_reset` sets ERASE+PROG, writes 0 to the page-aligned address. |

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
