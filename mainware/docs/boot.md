# mainware — boot chain + super-loop (`main`)

`main` (OEM `0x0803DEA8`) is the application capstone: a one-time clock +
peripheral + subsystem bring-up, followed by an infinite super-loop that ticks
every subsystem once per pass. It is the spine that drives `status_process`
(the behaviour engine, `docs/status-process.md`), `subsystem_update_sm` (OTA,
`docs/ota.md`), the modem SIM SM (`docs/modem.md`), the BLE/SSP bridges
(`docs/ble-commands.md`), and the lighting/LED engine.

**Sourced** as faithful C in `src/main.c` (+ `include/main.h`), adversarially
verified against the live disassembly. The OEM's own assert filename for this
translation unit is literally `"src/main.c"` (rodata `0x08053320`).

> **Link note.** `main` is defined `__attribute__((weak))` so the strong spin
> stub in `startup_stm32f413.S` remains the link entry: ~70 of main's callees
> are still unsourced, so binding a strong `main` would break the clean build
> with undefined references. The real loop is compiled + warning-checked but
> gc-sectioned, exactly like every other sourced-not-yet-rooted module. To root
> the call graph later: drop the startup stub and flip `src/main.c`'s `main` to
> strong.

## Entry + clock bring-up

1. `SCB->VTOR = 0x08020200` — re-point the vector table to mainware's own table
   (overrides the `0x08000000` that `SystemInit` wrote).
2. `hal_mcu_init` (`0x080232AC`) — HAL_Init equivalent: FLASH_ACR prefetch +
   I/D-cache, NVIC priority group 4, SysTick, SYSCFG + PWR clocks.
3. `cpsie i` (`enableIRQinterrupts`).
4. **Warm/cold dispatch:** if `*0x20000000 == 0x55AA55CF` → `boot_init_warm`,
   else `boot_init_cold`. The marker lives in retained low-RAM (below `.data`),
   so it survives a soft reset and selects the LSE-already-running path.

`boot_init_cold` / `boot_init_warm` configure the clock tree via the CubeF4
RCC routines (verified by struct contents, correcting the fan-out's names):

| step | fn | OEM | notes |
| --- | --- | --- | --- |
| OscConfig | `rcc_oscillator_config` | `0x0802643C` | PLLCFGR = PLLM 6 / PLLN 96 / PLLP 2, HSE-sourced; returns HAL 0/1/3 |
| ClockConfig | `rcc_clock_config` | `0x08027208` | ClockType 0xF, SYSCLK←PLL, APB1 /, **FLASH_LATENCY_3** |
| PeriphCLKConfig | `rcc_periph_clock_config` | `0x08026064` | RCCEx 88-byte block, `[0]=8`, `[9]=0x200`(cold)/`0x100`(warm) |

Both first enable the PWR clock (`RCC_APB1ENR.PWREN`) and select VOS scale 1
(`PWR_CR |= 0xC000`). **Cold** boots HSE+**LSI** (OscillatorType 9); **warm**
boots HSE+**LSE** (OscillatorType 5). Cold failure → `Error_Handler`; warm
failure → `muco_assert_fail("src/main.c", 0x55C/0x568/0x56E)`.

## Peripheral init sequence (one-time, in order)

`gpio_init` · `dma_controller_init` (DMA1/2 clocks + IRQ 0x0C/0x38) · the eight
serial ports · `i2c2_init` (400 kHz) · `i2c3_handle_init` · `tim1_pwm_init`
(period 2400, 3 OC channels) · `crc_init` · `adc1_init` (ch 4–7) · `tim6_init`
· `rtc_init` (24 h, 1 Hz from 32768) · `watchdog_init` (WWDG) · `tim7_init` ·
`tim10_init` · `comm_buffers_register_all` (17 comm buffers) · `scheduler_init`
· `button_press_state_machines_step` · `log_console_subsystem_init(0x55AA5501,
ctx)` (installs the UART log vtable into `g_log_func`) · `log_buffer_crc_check`
· `log_wake_reason` ("Wake Reason: %s") · `dma_peripheral_transfer_4word_step` ·
**`mainware_boot_init_sequence(ctx)`** · `uart_rx_ringbuf_get_byte(0)` ·
`app_ctx_ptr_set(ctx)` (publishes `ctx` into the holder `0x20000944`) ·
`clear_buffer_0x180` · `clear_buffer_0x600` · `HAL_GPIO_WritePin(GPIOD, PD7, 1)`
· `smodbus_queue_timer_init` ("smodbus_tmr") · `bmodbus_queue_timer_init`
("bmodbus_tmr") · `reset_reason_log_and_clear` (RCC/PWR reset flags) ·
`speed_capture_init(ctx+0x10B, ctx+0x31C)` (NVIC IRQ 0x17 input capture) ·
`flash_program_rdp_level_once` ("Set RDPLevel = %X").

### UART map (from the init functions)

| port | base | baud | init fn |
| --- | --- | --- | --- |
| USART1 | 0x40011000 | 115200 | `usart1_init` |
| USART2 | 0x40004400 | 115200 | `usart2_init` |
| USART3 | 0x40004800 | 9600 | `usart3_init` |
| UART5 | 0x40005000 | 115200 | `uart5_init` |
| UART4 | 0x40004C00 | 9600 | `uart4_init` |
| USART6 | 0x40011400 | 38400 | `usart6_init` |
| UART8 | 0x40007C00 | 115200 | `uart8_init` |
| UART7 | 0x40007800 | 115200 | `uart7_init` |

## `mainware_boot_init_sequence` (`0x0803FC94`)

Board + subsystem bring-up, run once. In order:

1. **Firmware banner** — `g_log_func("\r\nES3 v%d.%02d.%02d\r\n", …)` from the
   image-header version word at `0x08020004`. The firmware self-identifies as
   **"ES3"**.
2. **Power/LED GPIO rails** — a fixed sequence of `HAL_GPIO_WritePin` over
   GPIOA/B/D/E (PD15, PA12, PA15 pulse, PB3/10/9/15, PD10/11/12/13, PE5).
3. `display_module_init` ("ERR Led Display"), `flash_unlock_and_clear_status`,
   `module_ctx_init(ctx)`, LED-driver channel zeroing (`obj_set_field34/38`,
   `led_channel3_set_brightness`), TIM1 CH1/2/3 output enable, IRQ-10 timer
   start, and a paired peripheral disable.
4. **Self-test retry loop** (`do…while` on the retry budget byte `0x20000101`,
   counting device failures per pass; ≥3 failures → I2C bus-recover
   (`clock_pulse_gpioa8_until_pc9`, "Clocking %d") + retry; otherwise exit;
   `>2` final failures → set fault flag `0x800000`). Per pass it:
   - probes **HDC1080** temp/humidity (I2C 0x80, "ERROR HDC1080"),
   - reads the EEPROM id block + the persisted state record
     (`eeprom_read_config_with_crc_fallback`); on a bad/missing record it
     rebuilds defaults into `ctx+0x310…` and re-saves
     (`save_state_record_to_eeprom`, "Save default values"),
   - restores the flash config (`flash_read_config_with_crc_restore`); on
     failure → `settings_factory_reset`,
   - **migrates the config schema**: if `(ctx[0x140] & 0xFFFFFF) != 0x0706F4`
     it stamps `0x010706F4`, seeds defaults (speed 200, backup code), and
     persists both banks (`config_persist_dual_bank`, "res: %s"),
   - resets the per-loop context block (dozens of `ctx` fields),
   - wakes the **STC3115** gas-gauge (`stc3115_wake` → `stc3115_fuel_gauge_init`,
     "ERR ST3115 wake") — *note: the fan-out first mis-labelled `0x080398CE`
     "accel_disable"; the caller (success→gas-gauge init) proves it is the
     STC3115 wake,*
   - brings up the **LIS3DH** accelerometer (`lis3dh_accel_init` →
     `lis3dh_config_motion_int(0,6)`, "ERR LIS3DH"),
   - brings up the **MAX9768** audio amp (`audio_amp_init` →
     `amp_volume_brownout_apply`, "ERR init MAX9768"),
   - and the LED-matrix light sensor (`display_write_reg20_init`, "ERR Light sensor").
5. **SIM source detect** — read PE10: low → "SIM: PCB" (set PE12), high →
   "SIM: Holder" (clear PE12).
6. **Model string** — `snprintf(ctx+0x64A, 7, "%cS3.%c", region, ver)` where
   region is `'E'`/`'X'` (`ctx[0x10B]==1`?) and ver is `'2'`/`'0'`/`'1'`/`'?'`
   (warm-magic / `hw_version_lookup`).

### On-board I2C devices (from the self-test)

| device | role | I2C addr |
| --- | --- | --- |
| HDC1080 | temp / humidity | 0x80 |
| STC3115 | LiPo gas-gauge | (shared bus 0x20009B04) |
| LIS3DH | 3-axis accelerometer | 0x33 |
| MAX9768 | audio amplifier | 0x96 |
| 24-series EEPROM | state/config records | 0xA0 |
| LED-matrix controller | display + ambient light sensor | 0x60 / 0x66 / 0xBB-handle |

## Super-loop (`for (;;)`)

Per iteration, in order:

1. `watchdog_kick`.
2. `light_tick_update(ctx)`; if `ctx[0x34D]==0` → `modem_sim_state_machine`.
3. if `ctx[0x402]==1` → `light_pattern_step(ctx+0x350+i, i, ctx[0x102], ctx[0x10C])`
   for the three light channels.
4. `sms_info_tracking_state_machine` · `modbus_shifter_link_monitor` ·
   `modbus_bat_service_step` · `lipo_charge_state_monitor`.
5. `ble_ssp_dispatch` (≠0 → clear flag `0x800000`); `ssp_ble_tx_queue_pump`
   (≠0 → log `" ERROR SSP BLE msg removed"`).
6. `motor_fw_update_fsm_step()==3` → `sspm_rx_reply_handler` (≠0 → clear
   `0x400000`) + `sspm_tx_queue_pump` (==1 → log
   `" ERROR SSP MOTOR msg not confirmed"`).
7. `led_matrix_render_frame_region(ctx[0x354])` · `led_matrix_overlay_frame_region`
   · `dsp_recovery_telemetry_pump` (≠0 → `"ERR Display"`).
8. `button_press_state_machines_step` · `charger_and_pc1_sense_debounce(ctx+0x310)`
   · **`status_process(ctx)`**.
9. `ctx[0x3B0] = supply_voltage_sample_step()`; `ctx[0x3C0] =` either
   `output_value_filter_step()` or `ctx[0x3C4]` (gated by `ctx[0x3C6]`).
10. `ble_telemetry_change_broadcast(ctx)` · **`subsystem_update_sm(ctx)`**.
11. log-upload gate (`ctx[0x314]`/`ctx[0x313]`/`ctx[0x3B8]&0x800000` +
    `update_sm_is_idle`) → `log_upload_sm_step(flag)`.
12. `display_mode_sm_step(ctx)` · `factory_reset_sm_step(ctx)` ·
    `staged_msg_validate_and_dispatch(ctx+0x3D4)`.

The loop never sleeps — timed work runs under the SysTick/scheduler. The
application context is the flat struct at SRAM `0x200083A8` (`g_app_ctx`),
reached elsewhere via the holder at `0x20000944` and `g_app_state.ctx_sub`
(`struct session_ctx`, `app_state.h`); the three views share the same offsets.
