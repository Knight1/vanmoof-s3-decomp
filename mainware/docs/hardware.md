# mainware — hardware notes

The main controller application — the bike's central firmware. Runs on
the same MCU as `mainboot` (ST STM32F413VGT6, Cortex-M4F) and is the
first-stage's "Loaded Application" slot tenant. Mainware owns user
interaction (BLE app dialog, kick-lock, frame-lock, horn, sounds,
power-mode), orchestrates the per-subsystem updaters (Shifter, Motor,
Battery firmware push), and runs the cloud-comms uplink (uBlox modem
`AT+UHTTPC` to the VanMoof backend).

## Binary identity

| | |
| --- | --- |
| File | `mainware_1.07.06.bin` |
| Size | 218784 bytes (≈ 213 KB) |
| Version | `1.07.06` (Nov  1 2021 10:25:04) |
| SHA-256 | `e041e66a7110a2bbf6882317f865bfb7d5ba293a4149470cf6367aeb2649b8a1` |
| SHA-512 | `574ebb811eda88fffae05f21560f2183ea6e97a383c36e075217910c0771e9491860a612954d7a04e6c5cc77bb4e6e60adecbed7fd88dd7463f046e2ed550e90` |

This is the *first* (oldest) shipped mainware, chosen as the baseline
to mirror shifterware's `0.237` policy — the diff between 1.07 and the
final 1.09 is non-trivial (~28 KB removed between 1.08 and 1.09,
likely the modem/cloud path being torn out), so 1.07 is the right
starting point to see the system at its most complete.

## VanMoof container envelope

The `.bin` is not a raw flashable image — it has a 16-byte VanMoof
envelope plus a 496-byte build-info / padding region. The full first
512 bytes (file offset `0x000..0x1FF`) are still **flashed** at the
target slot — they sit *before* the vector table so that `mainboot`
can validate the slot and print the build-date banner without parsing
DWARF.

| File offset | Size | Field |
| --- | --- | --- |
| `0x000` | 4 | Magic — file bytes `55 AA 55 AA` (i.e. a little-endian `uint32` of `0xAA55AA55`; reproduced exactly by `startup_stm32f413.S`). Same magic across every VanMoof firmware image. |
| `0x004` | 1 | `0xF4` (unknown — constant across all four mainware versions) |
| `0x005` | 1 | Patch version (`0x06` for 1.07.06) |
| `0x006` | 1 | Minor version (`0x07` for 1.07.06) |
| `0x007` | 1 | Major version (`0x01` for 1.07.06) |
| `0x008` | 4 | CRC32 (VanMoof poly — see `vanmoof/crc.go`) |
| `0x00C` | 4 | Total image length (little-endian; equals file size) |
| `0x010` | 12 | Build date as ASCII (`"Nov  1 2021\0"`) — `__DATE__` literal |
| `0x01C` | 9  | Build time as ASCII (`"10:25:04\0"`) — `__TIME__` literal |
| `0x025` | 475 | `0xFF` padding up to the 512-byte VTOR alignment boundary |
| `0x200` | …   | STM32 vector table (initial SP, Reset, NMI, …) — start of code-image proper |

The version byte order (patch / minor / major) is the same as the
shifterware version-word encoding observed in the loader.

## MCU

**ST STM32F413VGT6** — confirmed from the `mainboot/docs/hardware.md`
identification work: Cortex-M4F, 100-pin LQFP, 1 MB flash, 320 KB
contiguous SRAM (`0x20000000..0x2004FFFF`).

Build flags: `-mcpu=cortex-m4 -mthumb`. The image's initial SP is
`0x20037000` (≈ 220 KB above SRAM base — leaves the upper 100 KB of
SRAM2 free; mainboot uses the same SRAM region for image staging
during updates). Whether to use `-mfloat-abi=hard -mfpu=fpv4-sp-d16`
will be decided once an FPU-touching function shows up; default to
soft float for now.

## Memory map (provisional)

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (mainboot)          | `0x08000000` | `0x08007FFF` |  32 KB — first-stage (sectors 0+1) |
| Flash (subsystem blobs / shadow) | `0x08008000` | `0x0801FFFF` |  96 KB — shadow app + shifter/motor/battery blobs (TBC) |
| Flash (mainware envelope) | `0x08020000` | `0x080201FF` | 512 B — 16 B header + build-date + padding (start of sector 5) |
| Flash (mainware code)     | `0x08020200` | `0x080556A0` | ~213 KB — vector table + .text + .rodata, spans sectors 5 + part-of-6 |
| Flash (free)              | `0x08055700` | `0x0807FFFF` | ~170 KB unused at top of flash (room for growth / per-MCU OTA cache) |
| SRAM (mainware)           | `0x20000000` | `0x20036FFF` | working set; initial SP at `0x20037000` |

### Known SRAM globals (from decomp)

| Address | Size | Symbol | Module | Notes |
| --- | --- | --- | --- | --- |
| `0x2000010E` | ≥6 | `g_state` | (not yet decoded) | Status/console block. `g_state[5]` (byte at `0x20000113`) is the login state machine: `0xFA` = ready-to-accept password, non-`0xFA` = locked-out / scheduler-slot id. |
| `0x20000000` | 4 | `g_boot_marker` | `main` | Warm-boot magic. `main` compares it against `0x55AA55CF`; match → `boot_init_warm` (skip cold init), else `boot_init_cold`. Lives in the **retained low-RAM** below `.data` (`0x20000000..0x20000013` — not touched by the `.data` copy or `.bss` zero), so it survives a warm reset. |
| `0x20000014` | 1 | `g_systick_step` | `systick.c` | Muco-runtime SysTick increment-per-tick (initially 1). **Same SRAM address as in mainboot** — both wares' `.data` starts at +0x14 from SRAM base. |
| `0x20000076` | 1 | `g_update_mode` | `app.c` | subsystem firmware-update mode (`+1` of a small control block at `0x20000075`); `update_mode_request` only overwrites it from idle (`==2`). |
| `0x20000288` | ≥7 | `g_announce` | `app.c` | broadcast dirty-flags block; `announce_mark` sets `+5` (channel 0) / `+6` (channel 1). |
| `0x200004C0` | 0x190 | `g_scheduler` | `scheduler.c` | Muco 48-slot one-shot scheduler table. **Two** 6-byte bitmaps: `allocated` at +0x00 (set by `scheduler_alloc`, cleared by `scheduler_release`) and `armed` at +0x08 (set by `scheduler_start`, cleared by `scheduler_release`, scanned by `scheduler_tick`); `callbacks[48]` at +0x10, `counters[48]` at +0xD0. |
| `0x200083A8` | ≥0x404 | `g_ctx` | `main` / `console.c` | the application/session context struct (`session_ctx`). `main`'s super-loop addresses it directly at this fixed address; the console reaches the same object through `g_app_state.ctx_sub`. Known fields: audio block `+0xF4..+0x10C`, volume `+0x104/5/6`, `[0x2D9]` logged-in, `[0x2E0]` fail-count, `[0x398]` service password, `[0x3D4]` SOC override; super-loop also touches `[0x34D]`, `[0x402]`, `[0x350-0x354]`, `[0x3B0]`, `[0x3B8]`, `[0x3C0-0x3C6]` (semantics TBD). |
| `0x20009368` | 4 | `g_app_ctx` | (not yet decoded) | application-state block; `+0x2DC` is `ctx_sub`, the pointer to `g_ctx` (`0x200083A8`). Through it the console reaches `[0x2D9]` logged-in flag, `[0x2E0]` failed-login counter, `[0x398]` service password. |
| `0x20009704` | 4 | `g_systick_counter` | `systick.c` | free-running SysTick counter |
| `0x20009D98` | 4 | `g_log_func` | `log.h` (extern) | `(*g_log_func)(const char *fmt, …)`-style logger. Referenced by `exceptions.c`, `panic.c`, `console.c`. Set once during init (initialiser not yet decoded). |
| `0x20000083` | 1 | `g_state_flag` | `app.c` | state/mode flag byte; `state_flag_get`/`state_flag_set`. `log_print_timestamp_prefix` saves/zeroes/restores it around a line; also written by `subsystem_update_sm` and the announce path. Plain byte, **not** an IRQ mask. |
| `0x200000E5` | 1 | `g_sms_track_state` | (modem) | `sms_info_tracking_state_machine` state byte (4-state SMS info-tracking scheduler). |
| `0x20000070` | 3 | `g_modem_at_timer` | `modem.c` | modem AT-engine scheduler-slot pair (`[0]` send, `[1]` response — reused by POWERON); `[2]` = AT-init command count (13 for SARA, 14 for LARA). |
| `0x20000760` | ≥0x1B4 | `g_update_sm` | `update.c` | OTA update state machine control struct (`subsystem_update_sm`): state byte `+0`, file count `+1`, file index `+2`, request scratch `+4`, PACK-header parse buf `+8`, per-bin claimed-flags `+0x14`, per-bin image table `+0x1C` (0x40 stride), flash dest/src/size `+0x1A0/+0x1A4/+0x1A8`, erase-ticks `+0x1AC`, commit-index `+0x19C`, retry `+0x19D`, scan-index `+0x19E`, BMS sub-state `+0x1B0..0x1B3`. |
| `0x20000079` | ≥9 | `g_update_slots` | `update.c` | OTA scheduler-slot handles (indices 1..8, `0xFA`=unallocated) + per-subsystem result/error code at `[2]`. |
| `0x20000294` | ≥0x22A | `g_modem_ctx` | `modem.c` | u-blox SARA modem working context: AT-engine state at `+0`, retry counter `+1`, AT tx scratch buffer `+0x18`, and a per-step `(substate,substep)` byte pair for every `modem_step_*` (`+0x14` POWERON, `+0x218` POWEROFF, `+0x21A..+0x229` SMS/PDP/PING/MESSAGE/LOCATION). |
| `0x20000914` | ≥0x2C | `g_adc_ctx` | `sensor.c` | ADC/sensor context **shared** by `supply_voltage_read` and `moving_avg10_push`: moving-avg write cursor (byte) `+0x00`, ten `u16` samples `+0x04`, ADC status byte `+0x22`, raw ADC sample (`u16`) `+0x2A`. |
| `0x20000944` | 4 | `g_app_ctx_ptr` | `app.c` | pointer to the app context used by `channel_resolve_status`; the three channel priority bitmasks are at `*ptr + 0xF4/0xF8/0xFC`. |
| `0x20001A44` | ≥0xB44 | `g_uart_ctx` | `uart.c` / `ssp.c` | UART/bus driver context; `+0xB3C` = TX ring-buffer pointer (`uart_send_byte`), `+0xB40` = RX ring-buffer pointer (`ssp_rx_byte`). |
| `0x20007E14` | 16×24 | `g_msg_tx_table` | (link) | second outbound message table (`maybe_enqueue_tx_message`): 16 slots × 24 B (`[0]`=type, `[1]`=in-use marker, `[3]`=handle, `[4..5]`=arg, `[6..7]`=len, `[8..23]`=payload). Distinct from the BLE/SSP `tx_queue` at `0x20008A40`. |
| `0x20009864` | 4 | `g_uart_dev_pp` | `uart.c` / `ssp.c` | pointer-to-pointer to the UART/bus peripheral block. `uart_send_byte` masks/sets bit 7 of `(*g_uart_dev_pp)+0xC` (TX-int); `ssp_rx_byte` masks/sets bit 5 of `(*(*g_uart_dev_pp))+0xC` (RX-int — a second deref hop). |
| `0x200099E4` | — | `g_rtc_handle` | (rtc) | STM32 HAL `RTC_HandleTypeDef` used by `rtc_fill_time_fields`. |
| `0x20009D90` | — | `g_crc_handle` | `crc.c` | STM32 CRC HAL handle (`CRC_HandleTypeDef`); field[0] → `CRC->DR`. Used by `crc32_hw_feed` and `HAL_CRC_Accumulate` (and passed by `flash_config_bank_write`). |
| `0x20037000` | ≥10 | `g_log_buffer_hdr` | `log.c` | circular SRAM log-buffer control header (just above `_estack`): 8-byte body + `u16` CRC-16 at `+8`, read/write cursors at `+0xC`/`+0x10`. `log_buffer_crc_check` validates it. |
| `0x20000029` | ≥0x40 | `g_sound_rec` / `G_STATE` | `app.c` / `states.c` | sound/clocking record (`channel_notify_emit`): bike-state byte at `+4`, saved state `+5`, aux `+9`, scheduler timer-slot id `+0xA` (`0xFA` = none). **Also the per-state-machine object** (`status_process`'s `G_STATE`): `+4` is the `switch` selector (the `alarm_state_name` code) and `+0x14..+0x3E` is a bank of per-state scheduler-slot handles (`0xFA`/-6 = unallocated). |
| `0x200001D8` | ≥0xB0 | `g_brownout_ctr` / `G_CLK` | `app.c` / `states.c` | brownout/clocking counter block; `+0xC` bumps on each amp-volume failure and triggers a `"Clocking %d"` log every third. `status_process` (`G_CLK`) also stores per-state flags + scratch here: `+0x7C/0x7D` shifter-on latches, `+0x80/0x84` last error-flag words, `+0x88` blink toggle, `+0x94` PC1 debounce, `+0xA1..0xAC` PIN-attempt counters + odometer-BCD digits. |
| `0x20000068` | ≥8 | `g_mode_state` | `app.c` | mode/state block: byte[0] = mode/sub-mode (`aux_mode_byte_get`/`set_mode_state_byte`; `enter_mode3_arm_show_timer` writes 3), byte[7] = scheduler slot id (`0xFA` = none). |
| `0x20009B04` | ~0x24 | `g_i2c3_handle` | `i2c.c`/`eeprom.c` | I2C3 HAL handle (`I2C_HandleTypeDef`, Instance `0x40005C00`); the EEPROM bus (`eeprom_write_region`) + the bit-bang recovery (`i2c3_handle_init`/`deinit`). |
| `0x20009728` | 0x14 | `g_wwdg_desc` | `watchdog.c` | WWDG refresh descriptor (built by `watchdog_init`): `{WWDG_CR 0x40002C00, 0x180, 0x7F, 0x7F, 0}`. `wwdg_hw_init` programs `WWDG_CR=0xFF` (T|WDGA) + `WWDG_CFR=0x1FF`; `watchdog_kick`→`wdg_reg_write_from_desc` reloads `WWDG_CR=0x7F` each loop. |
| `0x20000101` | 1 | `g_boot_retry_budget` | `main.c` | boot self-test retry budget. `mainware_boot_init_sequence` decrements it after an I2C-bus-error recovery pass (≥3 device failures); the do/while loop exits when it reaches 0 (set 0 directly on a clean pass). |
| `0x20009A84` | ≥0x40 | `g_led_pwm_obj` | `main.c` | TIM1 (`0x40010000`) handle / LED-driver object: `obj_set_field34/38` + `led_channel3_set_brightness` zero its `+0x34/+0x38/+0x3C` PWM-duty channels at boot; `tim_channel_enable_output(&obj, 0/4/8)` enables TIM1 CH1/2/3. |
| `0x20006E90` | ≥0xE80 | `g_bat_modbus_ctx` | `battery.c` | battery/BMS Modbus-RTU master context (slave 0xAA): master-SM state byte `[0]`, RX byte counter `[2]`, bus ring handle `[0xE74]`, transaction-SM state `[0xE78]`, the request/response **frame** at `[0xE7C]` (= `0x20007D0C`: `[0]`slave `[1]`func `[2..3]`reg(BE) `[0x84]`byte-count `[0x85+]`response `[0x105]`len `[0x106]`exception flag). Cleared by `bmodbus_queue_timer_init`. |
| `0x200000E7` | ≥9 | `g_bms_state` | `battery.c` | BMS lifecycle state object (`battery_telemetry_step`): substate byte `[3]`, scheduler-timer slots `[0]`/`[5]`/`[7]` (`0xFA`=unallocated), retry counter `[6]`, light-mode latch `[8]`. |
| `0x20008A00` | ≥0x40 | `g_batware_update` | `battery.c` / `update.c` | batteryware (BMS firmware) update record: arm flag `[6]`, status `[7]`, charger/FAULT-pin shadow `[0x3E]`. |

### C runtime (startup) layout

`Reset_Handler` (`0x08043E54`) is the standard CubeF4 reset stub. Its
literal pool fixes the linker symbols the future `startup_stm32f413.S` +
linker script must reproduce:

| Symbol | Value | Meaning |
| --- | --- | --- |
| `_estack`  | `0x20037000` | initial SP (= vector slot 0) |
| `_sidata`  | `0x08055534` | `.data` load address (flash, after `.text`/`.rodata`) |
| `_sdata`   | `0x20000014` | `.data` start (SRAM) — first word is `g_systick_step` |
| `_edata`   | `0x20000180` | `.data` end (`.data` is 0x16C B) |
| `_sbss`    | `0x20000180` | `.bss` start (= `_edata`) |
| `_ebss`    | `0x20009DB0` | `.bss` end (`.bss` is ~40 KB) |

After copying `.data` and zeroing `.bss`, `Reset_Handler` paints the whole
free-RAM region `[_ebss 0x20009DB0, _estack 0x20037000)` (~180 KB) with the
word `0x0000000E` — a stack/heap fill pattern (high-water-mark groundwork) —
then calls, in order:

1. `SystemInit` (`0x08043AA4`) — FPU enable + RCC reset + VTOR (see above).
2. `__libc_init_array` (`0x08020DF8`) — newlib CRT: preinit array, `_init`,
   init array.
3. `main` (`0x0803DEA8`, 613 B) — the application super-loop.

`.data` ending at `0x20000180` and `.bss` running to `0x20009DB0` means the
statically-initialised + zero-init working set is ~40 KB; the scheduler
table (`0x200004C0`) and the console/app-state globals all fall inside that
`.bss` span, consistent with their addresses.

STM32F4 1 MB sector layout: sectors 0..3 = 16 KB each, sector 4 = 64 KB,
sectors 5..7 = 128 KB each. Mainware's 213 KB image starts at sector
5 (`0x08020000`) — the first 512 bytes of that sector are the VanMoof
envelope (container header + `__DATE__`/`__TIME__`), and the STM32
vector table begins at offset `0x200` into the sector
(`0x08020200`). That 512-B prefix lets mainboot validate the magic
and print the build banner before bothering with VTOR.

## GPIO bring-up (`gpio_init`, `src/gpio.c`)

`gpio_init` (`0x080314E8`) enables the GPIO port clocks via `RCC_AHB1ENR`
(`0x40023830`) in the order **E, H, C, A, B, D**, sets initial output levels,
then configures each port through the CubeF4 `HAL_GPIO_Init`. Port bases (AHB1):
A `0x40020000`, B `0x40020400`, C `0x40020800`, D `0x40020C00`, E `0x40021000`.

Per-port pin masks (semantic per-pin roles still TBD — the masks are
reproduced verbatim in `gpio.c`):

| Port | Init level / mode | Pin mask |
| --- | --- | --- |
| E | outputs low | `0x102C` |
| E | output high | `0x40` |
| E | outputs (Mode 1) | `0x106C`; inputs `0x410` |
| B | outputs low | `0xC32F`; output high `0x400`; outputs (Mode 1) `0xC72F` |
| D | outputs low | `0xBCF0`; output high `0x1`; outputs `0xBCF1`; inputs `0x400E` |
| A | outputs low | `0x9000`; input `0x800`; outputs `0x9000` |
| C | alternate-function | `0x2F` (AF0), `0x10`, `0x100` (AF1), `0x400` (AF2) |

Identified concrete function: **PA8 = I2C3 SCL (bit-bang), PC9 = I2C3 SDA
(sense)** — `clock_pulse_gpioa8_until_pc9` (`0x0803C8F4`) is the I2C3 **stuck-bus
recovery** routine (deinit I2C3, pulse SCL up to 200× until SDA releases high,
re-init I2C3; the "Clocking %d" probe). I2C3 (`0x40005C00`, 100 kHz) is the
on-board EEPROM (AT24C, dev `0xA0`) bus; its HAL handle lives at SRAM
`0x20009B04`. The GPIOC AF pins are the other peripheral I/O (inter-module-bus
UART + the I2C3 normal SCL/SDA). The **window watchdog** (WWDG `0x40002C00`) is
refreshed each loop by `watchdog_kick` (writes `0x7F` to `WWDG_CR`).

### Motion sensor — ST LIS3DH accelerometer

The anti-theft motion sensor is an **ST LIS3DH** 3-axis accelerometer (confirmed
by the `status_process` log strings `"LIS3DH high sense"` / `"LIS3DH low sense"`).
`status_process` switches its sensitivity between a high-sensitivity standby/theft
mode and a ride mode, and consumes its motion interrupt as the `"Mems trigger"`
input to the alarm escalation (alongside the wheel-rotation sensor's `"Wheel
trigger"`). **I2C address 0x33** (WHO_AM_I reads back 0x33), with auto-increment
(`reg | 0x80`); brought up at boot by `lis3dh_accel_init` (`0x0803D0BC`, enables
**NVIC IRQ 0x48 + 0x49** for the two INT lines) → `lis3dh_config_motion_int(0,6)`.

### Cellular modem (u-blox SARA-G350) — power & control pins

The modem driver (`src/modem.c`, `docs/modem.md`) drives these lines (matching
the masks above): **PB4** main-supply enable (1 = on), **PA15** level-shifter
enable, **PE6** reset (1 = held; deasserted at power-on — the `E output high
0x40`), **PB0** PWR_KEY (held low, pulsed high ~150 ms to toggle the module — the
`B output high 0x400` is a sibling), **PB1** aux (held low), **PE10** SIM-present
detect (input, in the `E inputs 0x410` group; read by `sim_iccid_check`). The
modem-supply rail **Vgsm** is read via ADC (`adc_read_vgsm`); POWEROFF spins
until it falls below 200 mV. The AT channel is a dedicated UART (the SARA module).

### On-board peripherals & I2C devices (boot init, `main.c` / `docs/boot.md`)

`main` brings up the clock tree then ~30 peripherals; the bases + baud rates come
from the individual init functions (named this pass). Clock tree (cold/warm):
HSE + PLL (**M 6 / N 96 / P 2**), SYSCLK ← PLL, **FLASH_LATENCY_3**, VOS scale 1;
cold uses the **LSI**, warm the **LSE** (RTC already running). `SCB->VTOR` is
re-pointed to `0x08020200` as the first instruction of `main`.

| Peripheral | Base | Config | Init fn |
| --- | --- | --- | --- |
| USART1 | `0x40011000` | 115200 8N1 | `usart1_init` |
| USART2 | `0x40004400` | 115200 | `usart2_init` |
| USART3 | `0x40004800` | 9600 | `usart3_init` |
| UART4 | `0x40005000` | 115200 | `uart4_init` |
| UART5 | `0x40004C00` | 9600 | `uart5_init` |
| USART6 | `0x40011400` | 38400 | `usart6_init` |
| UART7 | `0x40007C00` | 115200 | `uart7_init` |
| UART8 | `0x40007800` | 115200 | `uart8_init` |
| I2C2 | `0x40005400` | 400 kHz | `i2c2_init` |
| I2C3 | `0x40005C00` | 100 kHz | `i2c3_handle_init` |
| TIM1 | `0x40010000` | PWM, period 2400, 3 OC ch | `tim1_pwm_init` |
| TIM6 | `0x40001000` | presc 66 / period 50 | `tim6_init` |
| TIM7 | `0x40001400` | presc 1199 / period 10000 | `tim7_init` |
| TIM10 | `0x40014400` | presc 95 / period 5000 | `tim10_init` |
| ADC1 | `0x40012000` | regular ch 4–7 | `adc1_init` |
| RTC | `0x40002800` | 24 h, 1 Hz (async 0x7F / sync 0xF9) | `rtc_init` |
| CRC | `0x40023000` | HW CRC-32 | `crc_init` |
| DMA1/DMA2 | `0x40026000/0400` | clocks + IRQ 0x0C/0x38 | `dma_controller_init` |
| WWDG | `0x40002C00` | reload 0x7F each loop | `watchdog_init` |

On-board I2C devices, probed in the `mainware_boot_init_sequence` self-test
(each failure increments a fault counter; ≥3 → I2C bus recovery + retry):

| Device | Role | I2C addr (8-bit) | Bring-up / fail log |
| --- | --- | --- | --- |
| HDC1080 | temperature / humidity | `0x80` (reg 0x02 config) | `hdc1080_write_config_reg` / `" ERROR HDC1080"` |
| STC3115 | LiPo gas-gauge (fuel) | shared bus (handle `0x20009B04`) | `stc3115_wake`→`stc3115_fuel_gauge_init` / `"ERR ST3115 wake"` |
| LIS3DH | 3-axis accelerometer | `0x33` | `lis3dh_accel_init` / `"ERR LIS3DH"` |
| MAX9768 | audio amplifier | `0x96` (config 0xD6) | `audio_amp_init` / `"ERR init MAX9768"` |
| AT24-series EEPROM | state + config records | `0xA0` (I2C3) | `eeprom_read_config_with_crc_fallback` / `"ERROR I2C eerom"` |
| LED-matrix controller | display + ambient light sensor | `0x60`/`0x66` (handle `0x20009BB8`) | `display_module_init` / `"ERR Led Display"`; light sensor via `display_write_reg20_init` / `"ERR Light sensor"` |

Other boot pins: **PD7** powered high after init; **PD15/PA12/PA15, PB3/9/10/15,
PD10/11/12/13, PE2/3/5** driven as power/LED rails; **PE2** = amp enable, **PD5**
toggled around the amp probe; **PE10** read as SIM-source detect (low →
`"SIM: PCB"` + set **PE12**; high → `"SIM: Holder"` + clear PE12); **PC8** read for
a state-record default. The firmware self-identifies as **"ES3"** (boot banner
`"ES3 v%d.%02d.%02d"`, model string `"%cS3.%c"` → e.g. `ES3.2`).

### Battery / BMS (inter-module Modbus, `docs/battery.md`)

The battery's BMS (a `batteryware` STM32L0) is **not** on I2C — mainware reaches
it as a Modbus-RTU master (slave **0xAA**, func 3 read / func 6 write, CRC-16
poly 0xA001) over the shared inter-module bus. Control/sense GPIO (from
`battery.c`):

| signal | pin | role |
| --- | --- | --- |
| BMS present | **PC10** (GPIOC, `0x400`) | pack-inserted detect |
| charger sense | **PC4** (GPIOC, `0x10`) | charging vs discharging |
| sense / FAULT | **PD1** (GPIOD, `0x2`) | BMS sleep / battery FAULT pin |
| BMS reset | **PB5** (GPIOB, `0x20`) | reset/power pulse |
| motor reset | **PB10/PB9** (GPIOB, `0x400`/`0x200`) | motor reset during pack bring-up |

Telemetry registers are unpacked (big-endian u16) into the app context at
`g_app_ctx + 0x3F2 + reg*2`; the batteryware register map (cells 1–10 at regs
27–36) is cross-validated from both sides.

## Vector table (head, from raw bytes — image @ flash `0x08020200`)

| Slot | Vector | Value | Note |
| --- | --- | --- | --- |
| 0  | initial SP | `0x20037000` | mid-SRAM stack base |
| 1  | Reset      | `0x08043E55` | thumb (low bit set) |
| 2  | NMI        | `0x0803C975` | distinct handler |
| 3  | HardFault  | `0x0803C989` |  |
| 4  | MemManage  | `0x0803C99D` | M4 fault — real on this part |
| 5  | BusFault   | `0x0803C9B1` |  |
| 6  | UsageFault | `0x0803C9C5` |  |
| 11 | SVCall     | `0x0803C9D9` |  |
| 12 | DebugMon   | `0x0803C9ED` |  |
| 14 | PendSV     | `0x0803CA01` |  |
| 15 | SysTick    | `0x0803CA15` |  |

The 9 system-exception handlers at `0x0803C975..0x0803CA15` sit on
20-byte boundaries — distinct functions, not the shared-trap pattern
seen in mainboot. That's consistent with an application built against
ST's CubeF4 startup template, which gives every exception its own
named handler (most just `while(1);`-loops).

These are now decoded into `src/exceptions.c`. Each logs its own name
through `g_log_func`: `NMI`/`SVC`/`DebugMon`/`PendSV` log and return;
`MemManage`/`BusFault`/`UsageFault` log and spin. `HardFault` is a naked
tail-call (`tst lr,#4; ite eq; mrs r0,msp/psp; b.w fault_dump`) into the
frame dumper at `0x0803CB6C`, which prints the 8-word stacked frame and
the SCB fault-status/fault-address registers before spinning:

| Register | Address | Label in dump |
| --- | --- | --- |
| CFSR  (Configurable Fault Status) | `0xE000ED28` | `CFSR = %x` |
| HFSR  (HardFault Status)          | `0xE000ED2C` | `HFSR = %x` |
| DFSR  (Debug Fault Status)        | `0xE000ED30` | `DFSR = %x` |
| MMFAR (MemManage Fault Address)   | `0xE000ED34` | `MMAR = %x` (OEM drops the F) |
| BFAR  (BusFault Address)          | `0xE000ED38` | `BFAR = %x` |
| AFSR  (Auxiliary Fault Status)    | `0xE000ED3C` | `AFSR = %x` |

`SysTick_Handler` (`0x0803CA14`) is the Muco tick wrapper:
`scheduler_tick()` then `systick_tick()`. The Muco runtime's fatal-assert
path `muco_assert_fail` (`0x0803DAC4`, `src/panic.c`) shares the same
`g_log_func` slot — it prints `"FATAL error File [%s] line [%d]"` and
spins; the independent IWDG reboots the board.

## Banner strings of interest (first pass)

These show the mainware's role and which subsystems it speaks to:

- `'MT' (@) 2019 STM32F4, Start` — Muco runtime banner (re-used from
  mainboot's library; mainware links against the same Muco runtime).
- `Motorpcb Application: v%x.%02x.%02X (%s %s)` — version report from
  the motor MCU (TI TMS320F28054F via Modbus).
- `../src/F2806/f2806x.c` — TI motor MCU driver code path.
- `Autobaud no answer`, `Err Autobaud [%d]` — UART autobauding to a
  peripheral (probably the uBlox modem).
- `AT+UHTTPC=0,5,"/bike-message","https",…` — uBlox SARA modem HTTP
  POST to VanMoof backend.
- `ASK APP to unlock` — BLE handshake with the iPhone/Android app.
- `Cartridge removed`, `Locked wake by mems`, `Wake from shipping` —
  power-management state machine, MEMS-based wake detection.
- `Charging liPo %d%%`, `External battery removed`, `Restore power level %d`
  — battery management.
- `PRESS_VERY_LONG BLE off`, `SOUND_S%c vol %d`, `Clear horn queue` —
  user-interface inputs and audio output.

## Provenance

Unlike `mainboot` (third-party Muco runtime + application logic
written by Muco for VanMoof), mainware is **VanMoof's own
application** built on top of the Muco runtime / Cube HAL. The
F2806/CC2642/STM32L0 subsystem drivers are clearly bespoke (no
upstream equivalent). The HAL layer underneath will be ST CubeF4 —
`HAL_FLASH_*`, `HAL_UART_*`, `HAL_CRC_*`, `HAL_GPIO_*`, etc. — and
gets marked `vendor-stock` when recognised, same as in `mainboot`.

## Open questions

- Exact flash slot layout (where do shifter/motor/battery blobs live,
  in what order, with what envelopes?).
- VTOR — **resolved.** `SystemInit` (`0x08043AA4`) writes the stock
  `VTOR = 0x08000000`, but `main` (`0x0803DEA8`) **re-points it on its
  very first instruction**: `SCB->VTOR = 0x08020200`. So the live vector
  table is mainware's own at `0x08020200`, as documented; there's just a
  brief window during early init (before `main`) where VTOR still points
  at mainboot's table.
- FPU usage — **answered: yes.** `SystemInit` sets `SCB->CPACR |=
  0xF00000` (CP10/CP11 full access), enabling the FPU. None of the
  *currently decoded* functions emit `vpush`/`vpop`, so they build fine
  under soft-float, but the materialised image will need
  `-mfloat-abi=hard -mfpu=fpv4-sp-d16` once an FP-using function is
  decoded.
- Modem AT command flow — is there a YMODEM-over-AT path, or only
  HTTP POSTs?
- BLE protocol with the CC2642 (which is itself running bleware) —
  probably Modbus framing same as the motor / shifter side.
