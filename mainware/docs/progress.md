# mainware — decomp progress

Target binary: `mainware_1.07.06.bin` (218784 bytes, ARM Cortex-M4,
STM32F413VGT6). Loaded into Ghidra at `0x08020000` so the 512-byte
envelope at file offset `0..0x1FF` lands at the start of flash sector
5, and the vector table at file offset `0x200` lands at `0x08020200`
— naturally 512-B-aligned for VTOR. Image spans sector 5 + part of
sector 6 (ends ~`0x080556A0`, ~213 KB).

See `docs/hardware.md` for the canonical binary identity, the envelope
format, the MCU identification work, and the version-history note that
picks 1.07.06 as the baseline.

## Decomp scope policy

Mainware is **VanMoof's own application** written by VanMoof (or
VanMoof contractors) on top of the Muco runtime + ST CubeF4 HAL. The
working policy is identical to `mainboot`:

- **Translate every function** that has observable behaviour
  (skip 2-byte `b .` trap stubs — they get listed as
  `decomp-asm`, one shared source line).
- **Recognise ST CMSIS / HAL / LL** stock functions when they
  appear and mark them `vendor-stock`. Mainware almost certainly
  links against `HAL_FLASH_*`, `HAL_UART_*`, `HAL_CRC_*`,
  `HAL_GPIO_*`, `HAL_TIM_*`, and a chunk of CMSIS-Core
  intrinsics. The build pulls those from a vendored Cube tree
  later.
- **Recognise Muco runtime** functions shared with `mainboot` —
  `systick_tick`, `scheduler_tick`, `systick_delay`, the
  RCC-reset and CRC helpers. These also become `vendor-stock`
  rather than getting re-decoded from scratch; same code, same
  expected names, just embedded in a different image.
- **Recognise libc** (`memcpy`, `memset`, `strlen`, `strcmp`,
  `printf` family) — these are pulled in from `arm-none-eabi-newlib`
  by the Cube build. Mark `vendor-stock` and supply from the
  vendored libc.

The bespoke layer worth understanding deeply is the **application
layer**: the super-loop / scheduler structure, the BLE / Modbus
command tables, the power-state machine, the modem driver, the
per-subsystem updater flows, and the bike-state model.

## Summary

| Count | Status |
| --- | --- |
| 411 | pending (auto-named `FUN_xxxxxxxx`) |
| 23  | vendor-stock — `strcmp`, `strtol`, `strlen`, `snprintf`, `memcpy`, `memset`, `__libc_init_array`, `_init`, `__getreent`, `malloc`, `free` (newlib), `__floatsidf` (libgcc); CubeF4 HAL: `HAL_GPIO_WritePin`, `HAL_GPIO_Init`, `HAL_GPIO_ReadPin`, `HAL_FLASH_Program`, `HAL_FLASH_Unlock`, `HAL_FLASHEx_Erase`, `FLASH_WaitForLastOperation`, `HAL_CRC_Accumulate`, `HAL_I2C_Mem_Write`, `HAL_I2C_Init`, `HAL_I2C_DeInit`; `memcmp` (newlib) |
| 0   | in-progress |
| 254 | decomp-c — `systick.c` (3), `console.c` (49), `scheduler.c` (7), `exceptions.c` (10), `panic.c` (2), `app.c` (16), `util.c` (4), `system_stm32f413.c` (1), `ssp.c` (9), `flash.c` (6), `crc.c` (3), `audio.c` (1), `log.c` (8), `sensor.c` (2), `uart.c` (1), `net.c` (2), `gpio.c` (1), `eeprom.c` (1), `i2c.c` (2), `watchdog.c` (5), `states.c` (2), `modem.c` (16), `ble.c` (1), `ble_read.c` (1), `update.c` (1), `main.c` (4), `battery.c` (18), `display.c` (33), `lighting.c` (8), `display_requests.c` (data: 38 descriptors), `shifter.c` (37) — see per-module log below |
| 1   | decomp-asm — `startup_stm32f413.S`: `Reset_Handler` (+ vector table, envelope, `Default_Handler`) |
| 125 | named (rename in Ghidra, no source yet) — (the **LED-matrix display engine** is now **SOURCED → `display.c`** (`display_module_init`/`display_send_init_cmd`/`display_write_reg20_init`/`display_panel_reset`/`led_driver_panel_config`/`led_driver_brightness_write`/`led_driver_standby_write`/`led_driver_set·enter_shipping_mode`/`led_matrix_render·overlay_frame_region`/`led_matrix_transmit_step`/`matrix_draw_speed·number·icon·level_bar·level_bar_blink`/`matrix_set_corner_led·turn_indicator`/`matrix_glyph_src_addr·frame_delay`/`display_mode_sm_step`/`display_request_*`/`is_display_bus_ready`/etc.) and the **lamp engine + ambient sensor** → **`lighting.c`** (`light_pattern_step`/`light_pattern_action_apply`/`light_sensor_read_step·i2c_read·fault_count_get`/`obj_set_field34·38`/`led_channel3_set_brightness`), with **~25 FUN_ helpers named this pass**, fan-out transcribed + adversarially verified (`docs/display.md`, `docs/lighting.md`). Two name corrections: `dsp_recovery_telemetry_pump`→**`led_matrix_transmit_step`** ("dsp" in `" ERR dsp freeze"` = display, not the C28x DSP); and `light_tick_update` (`0x080371E8`) identified as a **console↔shifter/battery UART bridge**, NOT lamps (still named, out of scope). — The battery/BMS Modbus driver — `modbus_bat_submit`/`bms_modbus_read`/`bms_modbus_write`/`bat_modbus_master_step`/`bms_telemetry_unpack`/`battery_telemetry_step`/`battery_state_process`/`battery_charge_display_step`/`modbus_bat_service_step`/etc. — is now **SOURCED → `battery.c`**; the shared bus CRC/TX helpers `bus_crc16_get`/`update`/`reset`/`verify` + `bus_tx_enqueue_byte`/`_n` were named this pass, extern pending a future `bus.c`. The spine `main`/`boot_init_cold`/`boot_init_warm`/`mainware_boot_init_sequence` **and `status_process`** are now **SOURCED** — `status_process` → `states.c` (the 62-case behaviour engine, adversarially verified), the rest → `main.c`; with their **~74 callees named this pass**, `docs/boot.md`: peripheral init `hal_mcu_init`/`dma_controller_init`/`usart1·2·3·6_init`/`uart4·5·7·8_init`/`i2c2_init`/`tim1_pwm_init`/`tim6·7·10_init`/`adc1_init`/`rtc_init`/`crc_init`/`comm_buffers_register_all`, clock `rcc_oscillator_config`/`rcc_clock_config`/`rcc_periph_clock_config`/`tim_channel_enable_output`, loop services `light_tick_update`/`light_pattern_step`/`modbus_shifter_link_monitor`/`lipo_charge_state_monitor`/`ssp_ble_tx_queue_pump`/`motor_fw_update_fsm_step`/`sspm_rx_reply_handler`/`sspm_tx_queue_pump`/`led_matrix_render·overlay_frame_region`/`dsp_recovery_telemetry_pump`/`charger_and_pc1_sense_debounce`/`supply_voltage_sample_step`/`output_value_filter_step`/`ble_telemetry_change_broadcast`/`update_sm_is_idle`/`log_upload_sm_step`/`display_mode_sm_step`/`factory_reset_sm_step`/`staged_msg_validate_and_dispatch`/`button_press_state_machines_step`/`app_ctx_ptr_set`, boot devices `display_module_init`/`hdc1080_write_config_reg`/`stc3115_wake`/`stc3115_fuel_gauge_init`/`lis3dh_accel_init`/`audio_amp_init`/`display_write_reg20_init`/`eeprom_read_id_block`/`eeprom_read_config_with_crc_fallback`/`flash_read_config_with_crc_restore`/`backup_code_init_default`/`region_speed_preset_table_load`/`flash_program_rdp_level_once`/`reset_reason_log_and_clear`/`log_wake_reason`/`log_console_subsystem_init`), OTA helpers (`flash_cache_disable`, `flash_cache_enable`, `download_chunks_pending_count`, `shifter_update_status_get`, `shifter_update_request`, `batteryware_update_status_get`, `batteryware_update_set_pending`, `bus_rx_byte_locked`), BLE (`maybe_enqueue_tx_message`), lock/alarm state (`bike_is_locked`, `ble_lock_state_get`, `ble_unlock_state_get`, `bike_state_is_standby`, `bike_status_coarse_get`), modem/tracking (`modem_sim_state_machine`, `sms_info_tracking_state_machine`), battery (`modbus_bat_service_step`, `modbus_bat_submit`, `modbus_shift_submit`, `battery_request_telemetry`, `bms_modbus_read`, `console_battery_dump`, `stc_read`, `gas_gauge_reset`, `batteryware_update_arm`), motor (`motor_get_timer_cb`), shifter (`shifterstatus_dump_v200`, `shifterstatus_dump_v201`), ADC (`hw_version_lookup`, `adc_read_vgsm`, `adc_read_5vsw`), console (`console_cmd_show`, `console_cmd_ver`), log (`log_buffer_dump`), flash/eeprom (`config_persist_dual_bank`, `flash_config_bank_write`, `save_state_record_to_eeprom`, `settings_factory_reset`, `reboot_restart_task`, `bat_reset_release_cb`), misc (`testmode_command_dispatch`, `rtc_fill_time_fields`), **+ 42 `status_process` per-state sub-handlers** (`status-process.md`): shifter-SM steps (`shifter_sm_get_step`/`set_step_3`/`_10`/`_13`, `shifter_get_active_flag`, `shifter_firmware_update_step`), `state_flags_set`/`clear`/`test` (64-bit flag pair ctx+0x3B8), LIS3DH (`lis3dh_int1_clear`/`powerdown`/`config_motion_int`, `accel_enable`), `locked_state_step`, `power_assist_gear_step`, `diagnostics_run_step`, `internal_lipo_charge_step`, `enter_stop_mode`, `system_reset` (NVIC), `led_driver_set`/`enter_shipping_mode`, `light_sensor_read_step`, `charge_level_adc_get`, `battery_on_detect_step`/`substate_advance`, `bms_write_reg8_and_poll`, `telemetry_datalog_emit`, `sched_timer_arm_or_alloc`, `set_unlock_state_persist`, `sms_track_state_get`, `state_table_ptr_get`, etc. |

`function_count = 814` per `ghidra/exports/mainware_program.json` (3 OEM functions newly
created this pass: `console_cmd_shipping`, `shiftdebug_pump_task`, `bat_reset_release_cb`).
The 49-command console dispatch table is mapped in `docs/console.md` — **all 49 handlers
decoded** (47 sourced into `console.c`, `show` + `ver` named+documented).
**The committed JSON is stale**: ~195 functions have been renamed + given
prototypes/no-return across the recent sessions — incl. the shifter pass (23
`FUN_*` renamed to `bus_queue_*`/`shifter_*`/`modbus_frame_flush` and the new
function `shift_rx_flush_timeout_cb` @ `0x08037410`, which the dump must
materialize) (everything in the Decoded,
Decomp-asm, Vendor-stock and Named tables below carries its OEM address, so
those tables are the authoritative name map until the JSON is regenerated).
The GhidraMCP server can't run the dump script — re-run
`ghidra/scripts/DumpMainwareProgram.java` in Ghidra to refresh it. The program
itself was saved after each session.

## Per-module decomp log

- `display.c` — **the LED-matrix display engine**, now SOURCED (33 functions,
  faithful C; fan-out transcribed + adversarially verified). The bike's signature
  dot-matrix panel: two IS31FL3236 LED drivers on I2C2 (halves @ device 0x60/0x66,
  IS31FL373x `0xFE`/`0xC5` unlock + `0xFD` page-select protocol), a dual-half
  framebuffer in `g_request_ctx` (`0x20008230`: half A `+0x01`, half B `+0x99`,
  flags `+0x130/+0x131/+0x132`), the bit-banged glyph/number/bar draw API (3-bit
  LUT + 5×7 glyph ROM at flash `0x0804F358`), the I2C-DMA frame transmitter with
  display-freeze bus recovery (`led_matrix_transmit_step`, logs `" ERR dsp freeze"`),
  and the ~40-case display-mode presenter (`display_mode_sm_step`). Full writeup →
  `docs/display.md`. **Name fix:** `dsp_recovery_telemetry_pump` → `led_matrix_transmit_step`
  ("dsp" = display). Assembly lessons: hoisted the shared LUT + glyph ROM to single
  file-scope tables (the agents' per-function inlines collided / shadowed);
  `display_module_init` returns `display_panel_reset()`'s status (r0 survives the
  flag stores) — the boot path branches on it; `uint`→`uint32_t`, local
  GPIO-init struct for the bus-recovery path. **Request descriptors materialized:**
  the 38 distinct flash animation/glyph descriptors that `display_mode_sm_step` /
  `matrix_draw_speed` fed to `maybe_set_pending_request` / `display_request_set` as
  bare magic addresses are now real C data in `display_requests.c` (named symbols,
  not hex); the three string args still passed as raw addresses
  (`"slow_show_speed_tmr"`, `"NAK\r\n"`, `" ERR dsp freeze\r\n"`) are now string
  literals.

- `display_requests.c` — **LED-matrix display request descriptors** (data only,
  no functions): byte-faithful C copies of the 38 flash rodata descriptors the
  matrix render/overlay pipeline consumes. Format decoded from
  `led_matrix_render_frame_region` + `matrix_glyph_src_addr`/`_frame_delay`: a
  `uint32` array `[origin_col, width, frame_count, frame_count×delay,
  frame_count×width packed-3bit-pixel words]`, length `3 + frames·(1+width)`
  words. Extracted from OEM `mainware_1.07.06.bin` (image base `0x08020000`);
  length formula verified across the whole contiguous `0x0804c104`→`0x0804da64`
  chain. 34,092 bytes total; each descriptor labelled `g_disp_req_<addr>` in
  Ghidra. (gc-sections out of the linked image like the rest — build still
  `text 1996`.)

- `lighting.c` — **the front/rear lamp engine + ambient light sensor**, now SOURCED
  (8 functions, faithful C; fan-out transcribed + adversarially verified). Three
  TIM-PWM lamp channels (CCR1/2/3 via `g_lights` @ `0x20006DC0`) with a ±1 fade
  engine and ten table-driven flash patterns (`light_pattern_step` /
  `light_pattern_action_apply`); the ambient sensor read path over I2C2 (device
  0x20, reg 0x50) with retry + `" ERR CM2323"` fault. Full writeup →
  `docs/lighting.md`. **Verify catch:** the fade-step callback was transcribed with
  swapped operands AND a dropped argument — the OEM calls the PWM callback with the
  just-updated brightness (`cb(ch[0x0D])`) and the fade-down test is `target <
  current`; both fixed. **Note:** `light_tick_update` (`0x080371E8`) is a console↔
  shifter/battery UART bridge, **not** a lamp function — left named, out of scope.

- `main.c` — **the application capstone**, now SOURCED (4 functions, faithful C,
  adversarially verified): `main` (`0x0803DEA8`, super-loop), `boot_init_cold`
  (`0x0803DDE0`), `boot_init_warm` (`0x0803DADC`), `mainware_boot_init_sequence`
  (`0x0803FC94`). Full writeup → `docs/boot.md`. The OEM's own assert filename
  for this TU is literally `"src/main.c"` (rodata `0x08053320`). Entry relocates
  `SCB->VTOR=0x08020200`, dispatches warm (`*0x20000000==0x55AA55CF`, HSE+LSE) vs
  cold (HSE+LSI) clock setup (PLL HSE/6×96/2, FLASH_LATENCY_3), runs ~30
  peripheral inits (8 UARTs, I2C2, TIMs, ADC1, RTC, CRC, DMA, WWDG, scheduler),
  then `mainware_boot_init_sequence` (banner `"ES3 v%d.%02d.%02d"`, GPIO rails,
  I2C device self-test HDC1080/STC3115/LIS3DH/MAX9768 with a fault-counted retry
  loop, state-record + config-schema defaults, SIM-source detect, model string).
  The infinite loop ticks lights → modem SM → shifter/BMS Modbus → BLE/SSP
  bridges → motor recovery → LED matrix → `status_process` → telemetry →
  `subsystem_update_sm` each pass. **Link:** `main` is weak so the startup stub
  stays the entry (closure not yet rooted) — build clean, `text 1996`. 31 strings
  byte-exact; the ~74 callees named in Ghidra (one verify-pass correction:
  `0x080398CE` is `stc3115_wake`, not the agents' "accel_disable" — proven by its
  caller gating `stc3115_fuel_gauge_init`). RCC names de-rotated:
  `0x0802643C`=OscConfig, `0x08027208`=ClockConfig, `0x08026064`=PeriphCLKConfig.
- `battery.c` — **the battery / BMS Modbus driver**, now SOURCED (18 functions,
  fan-out + adversarially verified; `docs/battery.md`). Modbus-RTU master to BMS
  slave **0xAA** (func 3 read / func 6 write, CRC-16 poly 0xA001) — the master
  side of the `batteryware` decomp, **cross-validated**. Core: `bms_modbus_read`/
  `bms_modbus_write` (PDU builders), `bat_modbus_master_step` (byte SM:
  request+CRC → response+verify), `bat_modbus_txn_pump`, `modbus_bat_service_step`
  (super-loop tick), `bms_telemetry_unpack` (register → app-ctx cache:
  reg N → `*(uint16_t*)(g_app_ctx+0x3F2+N*2)`, BE u16; serial reg 0xB → ctx+0x408),
  `battery_telemetry_step` (the ~16-state pack lifecycle: detect→bring-up→charge→
  reset→off), `battery_state_process`, `battery_charge_display_step` (`Set power
  state to %s`), `battery_on_detect_step`/`substate_advance`, `batteryware_update_*`.
  Ctx `g_bat_modbus_ctx`@`0x20006E90` (frame `+0xE7C`), BMS state @`0x200000E7`,
  update record @`0x20008A00`. GPIO: PC10 present / PC4 charger (GPIOC), PD1 sense
  (GPIOD), PB5 BMS-reset (GPIOB). The verify pass caught a badly-mangled
  `battery_telemetry_step` (its agent scrambled the per-case log strings + GPIO
  bases and dropped case-0xF's `bms_modbus_write`); re-decompiled + rewritten
  faithfully (strings resolved exactly, incl. the leading-space `" ERR battery
  FAULT PIN"`). Shared bus CRC/TX helpers named, extern (future `bus.c`). Build
  clean (gc'd, `text 1996`).
- `shifter.c` — **the shifter / drivetrain Modbus master**, now SOURCED (37
  functions, fan-out + adversarially verified; `docs/shifter.md`). The master
  side of the flagship shifterware: Modbus-RTU to the eShifter / MT-shifter
  (slave **0x20**, func 3/6/0x10, CRC-16 poly 0xA001) over **USART3** (9600).
  Layers: the shared frame-ring queue (`bus_queue_init/push/pop/reset/peek`, the
  same primitives `battery.c` externs — now defined here, so `modbus_shift_submit`
  finally has a body), the byte-level RTU engine (`shifter_modbus_rtu_step` +
  `shifter_crc_*` + locked USART3 byte I/O `shifter_uart_tx_byte/_buf/_rx_byte`,
  RX-flush watchdog `shift_rx_flush_timeout_cb`), the transaction pump
  (`shifter_modbus_pump` → `shifter_response_unpack`, which decodes func-3 replies
  into the session ctx +0x520.. and feeds the `shiftdebug` `"MBS 0x%04X 0x%02X"`
  stream), the link monitor (`modbus_shifter_link_monitor`, super-loop entry from
  `main`), the ~18-case drivetrain control SM (`shifter_control_step`: power-up,
  ID/HW handshake into ctx+0x520 = 0x200 std / 0x201 MT, auto-shift against the
  per-region speed-threshold tables at ctx+region*6+{0x10e..}/{0x126..}, manual
  shift-button path, MT zeropos calibration; gear actuator `shifter_send_gear`
  func-6 reg 2; `shifter_seq_status_poll_step` 18-state register sweep), the
  ~20-case OTA update SM (`shifter_update_sm_step`: probe → `pack_validate` the
  staged image at flash `0x08010000` → stream 0x20-byte blocks via func-0x10 reg
  0x82 → poll CRC/erase answer ctx+0x620 → reset + version readback → persist to
  EEPROM), the pre-check gate `shifter_firmware_update_step`, the SM accessors
  (`shifter_sm_get_step`/`set_step_3`/`_10`/`_13`, `shifter_get_active_flag`,
  `shifter_update_request`/`_status_get`), and the console dumps
  (`shifterstatus_dump_v200`/`v201`). SRAM: `g_shift_modbus_ctx`@`0x20005E3C`,
  `g_modbus_crc`@`0x2000008C` (+2 = RX-timeout slot `0x2000008E`),
  `g_shifter_sm`@`0x2000001C`, `g_shifter_ctx`@`0x2000019C`,
  `g_bus_ring_slots`@`0x20006E00`, `g_usart3_handle`@`0x20009824`,
  `g_usart3_rings`@`0x2000094C`; the SMs' `ctx` param = the session context
  `G_APP_CTX`@`0x200083A8` (= `g_app_state.ctx_sub`). **Verify catch (critical):**
  the adversarial pass found the two big SMs had their dense per-case log strings
  shifted by a literal-pool slot (and one `hdr[2]` that must be `hdr[10]` — a real
  stack overflow, since `pack_validate` writes a 0x28-byte header). I re-resolved
  **every** literal byte-for-byte from the OEM image and found the globals-agent
  table itself was wrong in 26 places (incl. the control-SM MT-case
  `"  ERR MS overflow"` the verifier hadn't flagged); all fixed against ground
  truth. `bus_queue_peek` returns the queued-frame count (not a frame word). 23
  `FUN_*` renamed in Ghidra + 10 SRAM/cb labels; build clean (gc'd, `text 1996`,
  `shifter.o` = 9551 B code).
- **Per-state engine** (`status_process` `0x0802AAF8`, ~14 KB) — decoded +
  documented (`docs/status-process.md`), the bike's central behaviour engine
  ticked every super-loop: dispatches on the bike-state byte and runs each
  state's logic (read **LIS3DH** accelerometer / wheel / buttons / BMS / charger,
  drive lights P1–PC + sound + motor/shifter, manage LiPo charge & CPU-stop/sleep,
  run the alarm/kick-lock/PIN flow + GSM tracking, finalize OTA/diag via
  `NVICReset`). ~80-callee surface; moves the state via `maybe_set_state_if_unlocked`.
  **Now SOURCED as full faithful C → `src/states.c`** (~2300 lines: a common
  prologue + a 62-case `switch` on `G_STATE[4]`, cases 0..0x3D = the
  `alarm_state_name` codes), via a region-fan-out transcribe→verify workflow
  (setting the 1-param prototype lets the decompiler render the whole function;
  the 62 cases were split across 9 agents, assembled, then adversarially
  verified — the verify pass caught 3 transcription slips: a deref-vs-address arg
  in case 9 and two copy-pasted log strings in cases 0x1b/0x26, all fixed).
  Model: the recurring DAT aliases collapse to `ctx` (param), `G_STATE`
  (`signed char *`@`0x20000029`, state byte `[4]`), `G_CLK` (`@0x200001D8`) and
  `g_log_func`; ARM unsigned-`char` made the `== -6`/`-0x12` slot sentinels need
  `signed char`. Build clean (gc'd, `text 1996`). The logic counterpart to
  `ble-commands.md`/`console.md` (inputs) and `state-machine.md` (positions).
- `update.c` — **OTA orchestrator** (`subsystem_update_sm` `0x08031900`) now
  **SOURCED as full faithful C** (771 lines) via source→verify workflow, with all
  62 log strings byte-exact and every callee resolved to a named extern (build
  clean, gc'd). The multi-subsystem firmware-update state machine (~22 states):
  updates all five firmwares from one BLE PACK package — `mainware.bin` (self →
  shadow flash `0x08060000` + `NVICReset` reboot to bootloader, "wait for BMS
  shutdown"), `motorware.bin` (`0x080A0000`), `shifterware.bin` (`0x08010000`),
  `batteryware.bin`/powerbank (Modbus 0xAA, `0x080C0000`), `bleware.bin` (OAD).
  Control struct `g_update_sm`@`0x20000760`, slots `g_update_slots`@`0x20000079`.
  Target-type map (ctx[0x32C+i]): 1=bleware, 2=mainware, 3=motorware,
  4=shifterware, 5=batteryware. 8 helper callees named; `memcmp` (`0x08020E60`)
  vendor. Per-target verify with `pack_validate` + version compare. Doc:
  `docs/ota.md`.
- `ble.c` / `ble_read.c` — the **BLE app-command surface**, both dispatchers
  sourced (were named+documented). `ble_cmd_dispatch` (`0x08033970`, ble.c) =
  the write/command switch (~40 `0x55xx` GATT-write cmds + low provisioning
  ids); `ble_read_request_dispatch` (`0x08034D20`, ble_read.c) = the read/
  telemetry twin (~50 char ids the app polls). Decoded by fan-out + adversarial
  verification against the live disassembly: the verify pass caught the
  ctx-holder-vs-ctx trap (cmd `0x5536` reads the eShifter-busy global at
  `0x20000948` = holder+4, NOT a ctx field), confirmed the `0x11A` MAC-string
  `snprintf` takes 3 args (real fmt `"%02X%02X%02XDeBug"`, no OEM bug), and the
  `0x55A2` testmode `memcpy` is 0x28 B. ctx model = `*(uint8_t**)0x20000944`;
  both loggers (`g_log_func` table[0]/[2]) collapsed to `g_log_func` per the
  console.c convention. Control flow / offsets / helpers / state transitions are
  verified. **Debug-log strings are now byte-exact OEM text** (resolved from the
  literal pools + rodata via the audit auto-names). Notable: the firmware's debug
  labels are frequently functionally shifted/stale — e.g. cmd `0x5566` (horn
  select) logs `"CMD_BLE_STATE_SOFT_RESET"`, `0x55A2`/`0x55A3` (testmode) log
  `"CMD_BLE_TOP/BOTTOM_HALF_OF_DISPLAY"`, `0x5521`/`0x5523` swap LOCK/ALARM, and
  `0x123` (BLE-chip reset GPIO) logs `"CMD_BLE_MUTE"`/`"Mute"`/`"Unmute"` — all
  reproduced verbatim. (The `maybe_set_pending_request` / `display_request_set`
  descriptor addresses are now **materialized** as real C data in
  `display_requests.c` — see the display.c log entry.) Maps:
  `docs/ble-commands.md`, UUID scheme `docs/ble-uuids.md`, states `docs/state-machine.md`.
- `modem.c` — the **u-blox SARA-G350 cellular modem** driver (the bike's
  phone-home / anti-theft tracking path). 16 functions sourced: the AT engine
  (`modem_at_exec` `0x0802F9BC`, `modem_at_response_match` `0x0802F3A0`), the 10
  per-state sub-state-machines (`modem_step_poweron`/`poweroff`/`sms_init`/
  `sms_read`/`sms_write`/`ctx_activate`/`ctx_deactivate`/`ping_send`/
  `message_send`/`location_send`, `0x0802FDC0`..`0x08030620`),
  `modem_sm_state_name` (`0x08033070`), `modem_registration_get` (`0x0803CDE0`),
  `modem_info_ready` (`0x0802AAE8`), and **`sim_iccid_check`** (`0x0802E328`) —
  the SIM lock that rejects any SIM whose ICCID ≠ `8931440400` (VanMoof
  Vodafone-NL). Outer orchestrator `modem_sim_state_machine` (`0x0803D284`)
  named + documented. Data labelled: `g_szIccidVanmoofPrefix` (0x080507E0),
  `g_szHostBikecomm`/`Ublox1`/`M2m`, `g_szAtUhttpcUpload`/`Bikemessage`,
  `g_pModemAtScript` (0x08043EDC). Full subsystem writeup → `docs/modem.md`.
- `console.c` — `volume_low_set`/`volume_medium_set`/`volume_high_set`
  (`0x080424A4`/`0x080423B8`/`0x080422CC`, 236 B each). Three near-identical
  command handlers (`vollow`/`volmid`/`volhigh`); differ only by which byte in
  the session context they write — `ctx_sub->volume_low/medium/high` at
  **`+0x105`/`+0x106`/`+0x107`**. *(Correction: the low/medium/high→byte binding
  is from the console command table @ `0x0804F5C4`; the `"Volume Low/Medium/High"`
  dump labels alone are off by one, which is why this was previously mis-mapped to
  `+0x104/5/6`. `+0x104` is the audio-config block start, not a volume.)* With no arg the
  handler echoes the current value; with an arg in `[0, 64]`
  (`"Volume 0..64"` range string) it sets the byte, drives the audio
  amp (PE2 = enable, PD5 = mute, written via `HAL_GPIO_WritePin`
  recognised as the CubeF4 BSRR helper at `FUN_08026AC6`), and runs
  an audio-engine apply through `FUN_08031728(ctx->cfg[0..3])`.
  Parsed `0` powers the amp down and prints `"Audio off"`. The OEM
  also snapshots `ctx_sub[0x104..0x1C4]` (192 B) onto the stack
  just before the apply call — the snapshot isn't read back in any
  decoded path, but it's preserved verbatim in our C in case some
  not-yet-decoded callee inspects the buffer. The C source factors
  the shared body into a static `volume_set_common(input, target)`
  helper, so the two public entry points are each 20 B trampolines;
  combined size is 256 B vs OEM's 472 B (saving 216 B on
  duplication). Behaviour-equivalent, not byte-equivalent.
- `console.c` — `console_start_motor_update` (`0x08042590`, 28 B).
  Logs `"Start motor update.."` and calls `FUN_080313E4(4)` (likely
  a subsystem-mode request — "4" is presumably the motor-update
  state). Single-line stub, byte-shape equivalent.
- `console.c` — `console_soc_set` (`0x080425AC`, 72 B). **Bound to the console
  `gear` command** (help "set gear") — a firmware relabel quirk: it parses an
  integer with `strtol(s, NULL, 10)`, writes it to `ctx_sub->set_soc` (`+0x3D4`),
  prints `"Set SOC %d"`, and calls `announce_mark(2)` to broadcast the change.
  No current-value echo when the argument is missing. (There is no `soc` command
  in the table; `console_soc_set` keeps its behaviour-accurate name.)
- `console.c` — `login_handler`. The ES3 debug-console login callback
  (entry `0x080425F4`, 166 B). Reads a NUL-terminated input line and
  matches it first against `g_app_state.ctx_sub->user_password` (the
  user-configurable service password at SRAM offset `+0x398`), then
  against the **hard-coded fallback password** baked into rodata at
  `0x080547EC` — the 40-character string
  `"vEVjGF!paYsM2EBV8SoDT8*T0eB&#T6xevaoxCaO"`. The fallback works
  unconditionally: standard `strcmp` (`FUN_08021428`, recognised as
  the canonical glibc/newlib optimised byte-then-aligned-word
  comparator) never reports a match between a non-empty input and an
  empty `user_password`, so when the user hasn't set their own
  password the first compare always falls through to the fallback
  compare. The `user_password[0] != '\0'` guard on the OEM-side after
  the first compare is therefore defensive dead-code in practice. On
  match the handler clears `fail_count`, sets `logged_in = 1`, and
  prints `"\r\nWelcome to ES3\r\n"`. On mismatch it prints
  `"Error login\r\n"`, increments `fail_count`, and on the 5th
  consecutive miss arms a 5-second lockout via the Muco scheduler
  (`scheduler_alloc` + `scheduler_start(slot, 0x1388, NULL)`); any
  input typed during the lockout window calls `scheduler_start` again
  on the held slot, re-arming the 5-second cooldown (anti-brute-
  force). The shape compiles to 292 B with `-Os` (GCC saves
  `r4-r8,lr` where OEM saved `r3-r5,lr` — same logic, fewer
  hand-optimised register reuses). Companion handlers at `0x080423B8`
  / `0x080424A4` / `0x08042590` (set-user-password, set-admin-password,
  set-baud) share the same line-read flow and are not yet decoded.
- `systick.c` — `systick_tick`. Identical shape to mainboot's
  `systick_tick`: increment `g_systick_counter` (uint32) by
  `g_systick_step` (uint8) on every SysTick interrupt. The step byte
  lives at SRAM `0x20000014` — **same address as in mainboot**, since
  both wares' Muco runtime starts `.data` at that offset. The counter,
  however, lives at SRAM `0x20009704` in mainware (vs `0x2000083C` in
  mainboot) — the same Muco library linked into a bigger image gets
  a different `.bss` placement. The instruction stream is byte-shape
  identical to mainboot (load counter-ptr, load counter, load step-ptr,
  ldrb step, add, str, bx lr); only the two literal-pool entries
  differ. The mainware **SysTick_Handler** at `0x0803ca14` is the
  Muco wrapper: `bl scheduler_tick; bl systick_tick`, with
  `scheduler_tick` at `0x080306d8` — a 48-slot version of mainboot's
  16-slot scheduler (table at SRAM `0x200004C0`, with the bitmap +
  callbacks + counters scaled up). Both `SysTick_Handler` and
  `scheduler_tick` are named but not yet sourced; the scheduler's
  larger table size means it can't share a `.c` file with mainboot's.

- `console.c` — `console_next_token` (`0x08040A5C`, 66 B). The line
  tokenizer every argument-taking command handler calls first. Skips the
  current token (delimiters space / `.` / `:`, terminator `\0`), then the
  run of delimiters, leaving `*pp` at the next token; returns 1 if one
  exists. Previously an `extern` stub in `console.c`; now sourced there,
  removing the stub.
- `console.c` — `console_region_set` (`0x080421CC`). The `region` command —
  the bike's speed/region mode (0=EU, 1=US, 2=JP, 3=OFFROAD; OFFROAD lifts the
  cap). See "Region / speed mode" above. Added `region` (`+0x109`) and
  `region_lock` (`+0x144`) to `session_ctx`.
- `scheduler.c` — the Muco 48-slot one-shot scheduler:
  `scheduler_tick` (`0x080306D8`), `scheduler_alloc` (`0x0803073C`),
  `scheduler_release` (`0x080307A8`), `scheduler_start` (`0x08030800`),
  `scheduler_slot_is_idle` (`0x08030838`), `scheduler_init` (`0x080306C0`,
  clears both bitmaps at startup). The slot table at SRAM
  `0x200004C0` (0x190 B) holds **two** 6-byte bitmaps — `allocated`
  (`+0x00`, set by `alloc`, cleared by `release`) and `armed` (`+0x08`,
  set by `start`, cleared by `release`, scanned by `tick`) — plus
  `callbacks[48]` (`+0x10`) and `counters[48]` (`+0xD0`). Allocation and
  arming are independent steps. `scheduler_tick` decrements each armed
  slot's counter (floored at 0) and fires its callback **once, when the
  counter lands on 1** (so a slot armed with N ticks fires after N−1
  SysTick periods; `login_handler`'s lockout uses a NULL callback and
  polls `scheduler_slot_is_idle`). On a full table `scheduler_alloc` does
  **not** return `0xFA` — it calls `muco_assert_fail("src/time.c", 127)`
  and never returns; a successful return is therefore always a valid id.
  (Corrects the earlier single-bitmap / "returns 0xFA" note.)
- `exceptions.c` — the nine Cortex-M4 system-exception handlers plus the
  HardFault frame dumper (`0x0803C974..0x0803CA1F` on 20-byte boundaries;
  `fault_dump` at `0x0803CB6C`). Built from ST's CubeF4 startup template,
  so each exception has its own handler. `NMI`/`SVC`/`DebugMon`/`PendSV`
  log their name via `g_log_func` and **return**; `MemManage`/`BusFault`/
  `UsageFault` log and **spin** (`b .`); `HardFault` is a naked tail-call
  (`tst lr,#4; ite eq; mrs r0,msp/psp; b.w fault_dump`) that hands the
  active exception frame to `fault_dump`, which prints the 8 stacked
  registers (R0-R3,R12,LR,PC,xPSR) and the SCB fault registers
  (`MMFAR`/`BFAR`/`CFSR`/`HFSR`/`DFSR`/`AFSR` at `0xE000ED34/38/28/2C/30/3C`)
  then spins. `SysTick_Handler` is the Muco tick: `scheduler_tick()` then
  `systick_tick()`. The naked HardFault and the `tst lr,#4` SP-select
  assemble byte-for-byte to the OEM; the log handlers are
  behaviour-equivalent.
- `panic.c` — `muco_assert_fail` (`0x0803DAC4`). The Muco runtime fatal
  handler called from ~10 sites (incl. `scheduler_alloc`):
  `g_log_func("FATAL error File [%s] line [%d]\r\n", file, line)` then a
  `b .` spin (IWDG reboots). Marked no-return; the format string lives at
  rodata `0x08053300`. `g_log_func` is the SRAM `0x20009D98` logger slot
  shared with the console and every exception handler.
- `memcpy` (`0x08020EC0`) recognised as newlib's optimised `memcpy`
  (unaligned head, 16-word unrolled body, word/half/byte tail; returns
  `dst`). Marked vendor-stock — supplies the snapshot copy in
  `volume_set_common`; `console.c` already calls `memcpy` via `<string.h>`.
- `app.c` — two application-core leaf primitives shared by the console and
  the super-loop. `update_mode_request` (`0x080313E4`) sets the subsystem
  firmware-update mode byte (SRAM `0x20000076`) **only when idle** (current
  value 2), so an in-progress update can't be preempted; callers are the loop
  state machine, the motor handler, and the `motorupdate` console command
  (mode 4). `announce_mark` (`0x0802F1C0`) sets a per-channel broadcast
  dirty-flag (SRAM `0x2000028D`/`0x2000028E` for channel 0/1); any other
  channel is a no-op. **Finding:** `console_soc_set` calls `announce_mark(2)`
  — a no-op — yet the SOC override still applies because the super-loop
  consumes `ctx->set_soc` (`FUN_08043C74(ctx+0x3D4)`) every iteration. The
  prior "announces via FUN_0802F1C0(2)" note was misleading; corrected.
- `systick.c` — the Muco timebase readers, siblings of `systick_tick`.
  `systick_now` (`0x080232F8`) returns `g_systick_counter` (ms since boot,
  1 ms/tick) — `systick_now` is byte-identical to the OEM. `systick_delay`
  (`0x08023304`) busy-waits `ticks` periods, rounding up by `g_systick_step`
  (DAT confirmed `0x20000014`) so the caller gets ≥`ticks` whole periods;
  `0xFFFFFFFF` blocks forever. These are the timebase the whole app polls
  against (the charger SM, `systick_delay(10/100)` pulses, the per-subsystem
  throttles via `FUN_08029B24`'s 100-tick sampler, etc.).
- `util.c` — pure packed-BCD <-> binary byte converters: `bcd_to_bin`
  (`0x0802311C`, `(bcd&0xF)+(bcd>>4)*10`) and `bin_to_bcd` (`0x08022F2E`,
  count-tens loop). RTC/time helpers; both transcribed from the OEM
  arithmetic. Found via an image-wide small-leaf sweep — most small leaves
  on this image are opaque state accessors, so these (and the two
  vendor-stock recognitions below) were curated rather than bulk-banking
  meaningless accessors.
- `system_stm32f413.c` — `SystemInit` (`0x08043AA4`), the stock CubeF4
  template: enable the FPU (`CPACR |= 0xF00000`), reset RCC (HSION, clear
  HSEON/CSSON/PLLON via `CR &= 0xFEF6FFFF`, `PLLCFGR = 0x24003010`, clear
  HSEBYP, `CIR = 0`), set `VTOR = 0x08000000`. Compiles to the OEM's exact
  operation sequence (behaviour-equivalent; the two-`bic` split matches).
- `startup_stm32f413.S` — the keystone. The 512-B VanMoof envelope
  (byte-perfect bar the post-build CRC/length), the 128-entry vector table
  (system slots → our handlers, IRQs → `Default_Handler`), and `Reset_Handler`
  (`0x08043E54`, asm: set SP, copy `.data`, zero `.bss`, paint `[_ebss,_estack)`
  with `0x0000000E`, then `SystemInit`→`__libc_init_array`→`main`). Plus weak
  `main`/`__libc_init_array` placeholders so the partial tree links.
- Vendor-stock recognised this sweep: `__floatsidf` (`0x08020980`, libgcc
  soft-float int→double — doubles go through libgcc since the FPU is
  single-precision) and `__getreent` (`0x08020DEC`, newlib `_impure_ptr`
  getter, `*0x2000011C`).

## Application structure (main / super-loop)

**`main` (`0x0803DEA8`) + the boot chain are now SOURCED → `src/main.c`** (full
map in `docs/boot.md`), adversarially verified behaviour-equivalent against the
disassembly. `main` is defined `__attribute__((weak))` so the strong startup
spin-stub stays the link entry until the ~70-callee closure is sourced — the
real loop compiles + warning-checks but gc-sections out (build stays clean,
`text 1996`); flip to strong + drop the stub to root the graph. The ~74 direct
callees were named this pass. The shape:

**Entry / one-time init.** First instruction writes `SCB->VTOR = 0x08020200`
— mainware re-points the vector table to its **own** table, overriding the
stock `0x08000000` that `SystemInit` wrote (resolves the VTOR open question).
Then `FUN_080232AC()`, `cpsie i` (enable IRQs), and a **boot-marker branch**:
if SRAM `0x20000000 == 0x55AA55CF` → `boot_init_warm` (`0x0803DADC`), else
`boot_init_cold` (`0x0803DDE0`) followed by the full ~40-call init sequence
(subsystem registrations `FUN_080332xx`/`FUN_0803c2xx`, GPIO/peripheral
bring-up, `HAL_GPIO_WritePin(GPIOD, PD7, 1)`, etc.). The marker lives in the
retained low-RAM region below `.data` (see `hardware.md`), so it survives a
warm reset and lets mainware skip cold init.

**Super-loop** (`do { … } while (true)`), per iteration, in order:
1. tick/poll (`FUN_080314D8`), per-subsystem service `FUN_080371E8(ctx)`;
2. if `ctx[0x34D]==0` → `FUN_0803D284()`; if `ctx[0x402]==1` → 3× `FUN_08037B64`
   (a 3-channel update, one per subsystem index 0/1/2);
3. SSP/messaging poll: `FUN_0803CC6C`, `FUN_0802945C`, `FUN_0803F338`,
   `FUN_08036B98`; BLE-message check `FUN_0803F8FC` → event `0x800000`;
   `FUN_0803F6B4` → logs `" ERROR SSP BLE msg removed"`;
4. charge state `FUN_08030FF4()==3` branch → motor-message confirm
   (`FUN_0803A278`), logs `" ERROR SSP MOTOR msg not confirmed"`;
5. display `FUN_0803BB40()` → logs `"ERR Display"`;
6. battery/voltage update: `ctx[0x3B0]=FUN_08029B24()`; `ctx[0x3C0]` =
   `FUN_08038F78()` or `ctx[0x3C4]` depending on `ctx[0x3C6]`;
7. lock/kickstand logic (`ctx[0x313]`/`[0x314]`/`[0x3B8]&0x800000`,
   `FUN_08032980`) → `FUN_08029774(flag)`;
8. tail: `FUN_0802E800(ctx)`, `FUN_08038A90(ctx)`, `FUN_08043C74(ctx+0x3D4)`
   (consumes the SOC override).

The event log strings are reached via `g_log_func` (`0x20009D98`). The
super-loop never sleeps — it's a busy poll with the scheduler/SysTick
driving timed work underneath. The application context is the global struct
at SRAM `0x200083A8` (same object the console reaches through
`g_app_state.ctx_sub`; the `+0x3D4`/audio-block offsets line up). The next
sub-targets are its leaf callees (e.g. the SSP poll helpers and `FUN_08043C74`).

## Subsystem firmware update (OTA orchestrator)

`subsystem_update_sm` (`0x08031900`, called from the super-loop with the
context) is **mainware's whole-bike firmware-update engine** — it pushes new
firmware to every other MCU on the bike over the inter-module bus. A ~22-state
machine (state byte at `*DAT_08031bbc`):

- **Manifest + target identification.** It walks a list of pending firmware
  files (`count` at `DAT_08031edc[1]`) and identifies each target by matching
  the **2-char prefix of its filename** (`FUN_08020E60(slot, id, 2)`) against
  rodata at `0x08050FF4+`: `"ma"`→mainware, `"mo"`→motorware, `"sh"`→shifterware,
  `"ba"`→batteryware, `"ble"`→bleware. The matched type selects the per-subsystem
  mode written to `ctx[0x32C + idx]` (1..5) and a target flash base / size cap
  (`0x40000` / `0x20000` / `0x10000` per type).
- **Transfer.** Modbus/SSP writes via `FUN_0803F9CC(reg, len, buf, …)` — control
  registers `0x104` (update command), `0x110` (mode/abort), `0x11B`/`0x11C`
  (finalize) — interleaved with block read/write (`FUN_0803CF30`/`FUN_0803CF94`/
  `FUN_0803CFD8`) and `scheduler`-driven timeouts. Progress is logged as
  `"Process: %s %d bytes on 0x%08X"`; `"Shifter power on"` powers a target up
  first.
- **Entry/exit.** State `0xB` consumes `update_mode_request(4)` (the value the
  `motorupdate` console command sets — see `app.c`) and only proceeds when a
  subsystem reports update-ready (`ctx[0x402]==1 || ctx[0x400]==1`). State
  `0x15` = abort/error (calls `FUN_08029CA0(code)`), `0x16` = done/cleanup; the
  final stages can trigger a self-reset of mainware (`0x16`/case 2: DSB +
  AIRCR write + spin).

The **full pipeline lives in this one state machine** (the strings all xref
back into it):
1. **Receive (BLE).** `"Start BLE update"` → packets arrive via `ble_ssp_dispatch`
   (`0x0803F8FC`) from the bleware MCU over the bus. Each packet header is
   validated (`"Invalid packet header"`, `"No packet found"`), with CRC
   (`"ERR BLE-CRC"`) and per-packet timeouts (`packet_tmr`, `between_pack_tmr`).
2. **Stage.** Firmware is written to **shadow flash** (`"Erasing shadow flash
   %d Kb"`, `"Flash write error %d"`) and the **update table / manifest** is
   built (`"Invalid update table"`) — the per-file list `subsystem_update_sm`
   then walks (matching `*ware.bin` prefixes).
3. **Flash each subsystem** over the bus: Shifter (`"Send Shifter erase"`,
   `"Shifter CRC error"`, `"crc check ok"`), Battery/BMS (`"Send battery erase"`,
   `"BMS CRC error"`, `"Wait for BMS internal update!"`), Motor
   (`"Update F2806 CPU"`, the TI C2000), BLE. Each: erase → write blocks →
   CRC-check → done, with `"No need for X update"` skips and vbat guards
   (`"Shifter update no vbat (%d)"`).
4. **Mainware self-update** can't flash itself live: `"Update Main by reboot,
   wait for BMS shutdown"` — it reboots so the loader (muco-boot) applies the
   staged image. `"OAD_UPDATE"` is the bike state during all this.

This ties the console, the loop, and the bus together: **`motorupdate` →
`update_mode_request(4)` → `subsystem_update_sm` flashes the named MCU**. It's
deep (dozens of callees) so it stays mapped-not-sourced, but it's the single
most important behaviour in the firmware for the "understand, then modify" goal.

### Cloud / modem (tracking & telemetry — NOT the OTA channel)

**Correction:** the uBlox cellular modem is the **anti-theft tracking /
telemetry** uplink, *not* the OTA firmware-download channel. OTA firmware
arrives over **BLE** (phone app → bleware on the CC2642 → inter-module bus →
mainware → `subsystem_update_sm`). The modem path's request builders are thin
`snprintf` wrappers around the two backends:
- `bikecomm_request_build` (`0x0802F8A0`) → `"bikecomm.vanmoof.com"` (`0x08050890`)
  — the bike↔cloud message channel (telemetry, location, commands).
- `ublox_request_build` (`0x0802F940`) → `"ublox1.vanmoof.com"` (`0x080508C0`)
  — the modem HTTP endpoint.
Both call `snprintf` (`0x080212B0`, newlib vendor-stock); the modem driver
(`AT+UHTTPC`) transports them. The SIM is checked against an expected ICCID
(`"Wrong iccid, %s"`, `FUN_0802E328`). These belong to the **tracking** subsystem.

**OTA download (BLE) — mapped.** The firmware bytes come from the phone app over
BLE; `ble_ssp_dispatch` (`0x0803F8FC`, called from `main`'s super-loop) is the
BLE/SSP **message receiver** — it pulls messages bridged from the bleware MCU
across the inter-module bus (logs `"ERR BLE SSP packet not in queue"`). Firmware
packets flow through it into `subsystem_update_sm`, which does the receive →
stage → flash (see the dedicated section above and below).

## Diagnostics / error flags

The bike's fault state lives in two words of the application context:
- **`ctx[0x3BC]` — the 32-bit error/fault-flags word.** Each set bit is a
  distinct fault. The "Error Flags" report (inside `status_process`, around
  `0x0802ACE2`) rotates the word bit-by-bit and logs each set bit's index via
  `g_log_func("Error Flags: %d", bit)`, or `"Error Flags: None"` when clear.
- **`ctx[0x3B8]` — a status word.** Its `0x800000` bit gates the kickstand/lock
  decision in `main`'s super-loop.
- **Diag self-test** (around `0x0802D540`) loads both as a 64-bit pair
  (`ldrd [ctx,#0x3B8]`): `(status | errors) == 0` → `"Diag ok"`, else
  `"Diag fail"` plus it raises an error state (`0x22`) and an action.

These are emitted by **`status_process`** (`0x0802AAF8`, ~14 KB) — the large
per-loop bike-status/event processor `main` calls each iteration (named,
**not sourced**: the function is too big for the decompiler and likely merges
several logical handlers). **Struct note / TODO:** `ctx[0x3B8..0x3C7]`
(status/error/battery fields) fall inside the range I tentatively modelled as
`session_ctx.user_password[0x3C]` (`+0x398..+0x3D3`) — so the password buffer is
actually shorter (≤ `0x20`, `+0x398..+0x3B7`) and the status/error words follow.
`app_state.h` is left unchanged pending a confident re-model (the fields also
overlap union-style: `ctx[0x3C4]` u32 vs `ctx[0x3C6]` u16).

### Region / speed mode (`region` console command)

`console_region_set` (`0x080421CC`, sourced) sets `ctx_sub->region` (`+0x109`):
**0=REGION_EU, 1=REGION_US, 2=REGION_JP, 3=REGION_OFFROAD**. EU/US/JP are the
regulated speed caps; **OFFROAD removes the cap**. A change is accepted only when
the lock state `ctx_sub->region_lock` (`+0x144`) is < 3 (states: 1=off-road
disabled, 2=locked, else unlocked) — that byte is set elsewhere (likely the
BLE/app or cloud lock path, not this command). Applying a region re-runs the
config-apply `FUN_08031728`, which pushes the `ctx_sub[0xF4..0x103]` block to the
drive subsystem — i.e. the speed cap is enforced by the **motor controller**, and
this command is what reconfigures it. `alarm_state_name` (`0x08032DF0`, was
`lock_state_name`) is the `state→name` table (62 entries; 0–3 = `ALARM_*`) used
for logging alarm/security states.

### Init functions decoded this pass

- `gpio_init` (`0x080314E8`) — board GPIO bring-up: enables the GPIO port
  clocks (RCC AHB1ENR bits), sets initial output levels via `HAL_GPIO_WritePin`
  on ports D/E/etc. (masks `0x102C`, `0xC32F`, `0xBCF0`, `0x9000`…), then
  configures each pin's mode/speed/pull via `FUN_080267D0`. The authoritative
  pin map for mainware — sourcing it (and `FUN_080267D0`) will populate
  `hardware.md`'s GPIO table.
- `config_crc_check` (`0x0802968C`) — validates an 8-byte config block with a
  CRC-16 (`FUN_0803C2C8(p, 8, 0xFFFF)`) against the stored value at `+8`; on
  mismatch it resets the block (`FUN_0802957C`) and re-persists it. Config
  integrity guard, run during init.

## BLE / SSP transport & command set (`ssp.c`)

The phone app's BLE link is bridged by the bleware MCU onto the inter-module
bus; mainware receives it as the "SSP" stream. The transport is now sourced:

- **`slip_rx_packet`** (`0x0803F5A4`, `ssp.c`) — a **SLIP de-framer** (RFC-1055:
  `0xC0` END, `0xDB` ESC, `0xDC`/`0xDD` escaped END/ESC) with a **CRC-16**
  trailer (`crc16`, `0x0803C2C8`, the Modbus-bus CRC). Bytes are popped one at a
  time from the RX ring by `ssp_rx_byte` (`0x08036528`). Returns 0 = complete
  valid frame, 2 = CRC/framing error (`"ERR BLE-CRC"`, `"BLE-SEQ"`), 1 =
  receiving. The de-framer state lives at the SSP context `0x20008A40 + 0x600`.
- **`ble_ssp_dispatch`** (`0x0803F8FC`, `ssp.c`) — pulls one frame and dispatches
  by message **type** (byte [1]): `0x05` = command (`ble_command`), `0x06` =
  prepare/length, `0x07` = data (command id [3..4] + payload [7..] →
  `ble_cmd_dispatch`). Sends a `{1,5,id}` ack. Called every super-loop pass.

**`ble_cmd_dispatch`** (`0x08033970`, named — a ~2.5 KB switch, mapped not
sourced) is the **BLE app-command surface**: a big switch on the 16-bit command
id. Highlights decoded from it:
- `0x5521` lock/unlock, `0x5523` power-state set (off/standby/on…), `0x5562`
  unlock-with-state-checks.
- **`0x5534` / `0x5535` / `0x553C` = region/speed over BLE** — the app
  equivalent of the console `region` command: sets `ctx[0x109]`/`ctx[0x3C9]` and
  the speed cap `ctx[0x354]` (region 1→180, 2→120, 3→60, 4→30 in its unit; `0xFF`
  → OFFROAD). So the speed limit is settable from the app, gated by the same
  `ctx[0x144]` lock.
- `0x5503` SOC, `0x5537` assist-curve, `0x5566`/`0x5574` power mode, `0x5582`
  LEDs, `0x5564` alarm, `0x5572` set module key (`ctx[0xF4..0xFC]`), `0x55C1`
  log control. Most handlers end by echoing back via `FUN_0803F9CC(id, …)` (the
  bus reply) and re-applying the config block (`FUN_08031728`).

The OTA firmware **data** path also enters here (type 0x07 / the update command
ids `0x104`/`0x110`/`0x11B`); the staging into shadow flash is now decoded — see
below.

### BLE app-command map (`ble_cmd_dispatch`, `0x08033970`)

`ble_cmd_dispatch` is the **production control surface** — the ~40-case switch on
the 16-bit command id that turns phone-app GATT writes into bike actions (the
parallel to the debug console). It's named + documented (deep switch, not
sourced); the full **command→action table is in `docs/ble-commands.md`**.
Highlights: `0xFA` power on/off; `0x5521`/`0x5523`/`0x5562` lock / unlock / alarm
arm; `0x5535`/`0x553c` region + speed mode (+ region-lock); `0x5534` motor power
level; `0x5503`/`0x5572` backup code; `0x5533` units; `0x5581` light mode; `0x5582`
LEDs; `0x5524` alarm; `0x5536` shift gear; `0x5537` speed moments; `0x5538`
transmission; `0x5564` wheel size; `0x5571` play sound; `0x55C1` log-to-app;
`0x11A` provisions the BLE MAC + serial; `0x55A2`/`0x55A3` arm test/CRC modes. Each
write mutates `ctx` field(s), `config_persist_dual_bank` / `save_state_record_to_eeprom`,
gives UI/sound feedback (`channel_notify_with_status`), and acks via
`ssp_ble_enqueue_tx_packet(cmd, …)`. The read twin (phone polling state) is
`ble_read_request_dispatch` (`0x08034D20`). The `ctx` offsets line up with the
`show` dump + `console.md` (region +0x109, backup code +0x100, units +0x10A,
light +0x10C, wheel +0x10B, distance +0x31C, alarm +0x317, power level +0x3C9).

The **read/telemetry twin** `ble_read_request_dispatch` (`0x08034D20`) is also
mapped (the `## Read / telemetry surface` table in `docs/ble-commands.md`) — what
the app polls to render bike state (battery summary 0x5541, error flags 0x5563,
buttons 0x5568, version strings 0x5549–0x5550, plus the read-back of every config
write). The lock/alarm reads collapse the fine state byte into coarse app enums
via `ble_lock_state_get`/`ble_unlock_state_get`/`bike_status_coarse_get`/
`bike_state_is_standby` (+ `bike_is_locked`, the lock-pin PC8 + flag check).

**The bike lock/alarm state machine is documented in `docs/state-machine.md`**:
the state byte @ SRAM `0x2000002D` holds the `alarm_state_name` codes (0..0x3D,
grouped into boot/ride, standby/lock, unlock, PIN, alarm/tracking, shipping,
power/sleep, charging, OTA, diag); `maybe_set_state_if_unlocked` is the write gate
(refuses to overwrite shipping states 6/7); and the BLE/console commands are the
transition inputs (e.g. `0x5523`→alarm/lock, `0x5521`→lock/unlock, `0xFA`→power,
`setoad`→OAD `0x19`, `shipping`→`0x07`).

## Shadow-flash staging (`flash.c`)

Once a firmware **PACK** is received and validated, `subsystem_update_sm` erases
the destination sectors and programs the image. Decoded:

- **`pack_validate`** (`0x0803CFD8`, sourced → `flash.c`) — the **PACK validator**.
  The PACK begins with the VanMoof magic `0xAA55AA55` (`DAT_0803D068`, the same
  magic as the image envelope); rejects length ≥ `0x40000` (256 KB). It resets the
  CRC unit, then runs the **hardware CRC-32** (`crc32_hw_feed` `0x0802320E`, STM32
  CRC peripheral) over the 10-word header (with the stored-CRC + length words
  masked to `0xFFFFFFFF`) immediately followed by the body — `(len − 0x28)/4`
  words, one continuous CRC — and compares it to the stored CRC `pack[2]`. Returns
  0 = valid, 1 = CRC mismatch, 2 = bad magic / too big.
- **`flash_erase`** (`0x0803CF30`, sourced → `flash.c`) — erases every flash
  sector overlapping `[addr, addr+len)`: `flash_addr_to_sector` (`0x0803CE14`,
  sourced) bounds the range, `HAL_FLASHEx_Erase` (`0x080235B4`, CubeF4 HAL with a
  sector `EraseInit`) clears each, kicking the watchdog between sectors. (Behind
  `"Erasing shadow flash %d Kb"`.)
- **`flash_write`** (`0x0803CF94`, sourced → `flash.c`) — masks IRQs, waits for any
  in-flight flash op (`FLASH_WaitForLastOperation` `0x08027B80`), clears `FLASH_SR`
  (`0x40023C0C`←`0xF3`), then programs the image one **32-bit word** at a time via
  `HAL_FLASH_Program` (`0x08027BE0`, the width-tagged HAL — op 2 = word; op 0/1/3 =
  byte/half/dword). Both compile faithfully; `flash_write` even reproduces the OEM
  quirk of leaving IRQs masked on a program error.

So the OTA write path end-to-end: **BLE SLIP frame → `ble_ssp_dispatch` →
`pack_validate` (magic + CRC-32) → `flash_erase` (sectors) → `flash_write`
(words) → `subsystem_update_sm` pushes/verifies each subsystem.**

## BLE/SSP TX, state persistence & telemetry (workflow batch)

A fan-out workflow analysed 19 functions (10 returned, adversarially verified);
8 were sourced and the rest named. Highlights, including two corrections to
earlier guesses:

- **BLE/SSP TX queue.** `ssp_ble_enqueue_tx_packet` (`0x0803F9CC`, sourced →
  `ssp.c`) is the **TX twin** of `ble_ssp_dispatch` (RX): a 128-slot queue of
  12-byte descriptors `{flags, type=5, seq, cmd16, len16, payload_ptr}` with a
  rolling unique sequence id. This confirms `0x20008A40` is one unified SSP
  context — the 128×12 = 0x600-byte TX queue occupies `+0x000..+0x5FF`, the
  SLIP de-framer state is at `+0x600`, the ack frame at `+0x710` (the `_pad0`
  in the earlier `ssp.c` model was the TX queue). **Correction:** `0x0803F9CC`
  is the *BLE SSP* enqueue (assert file `"src/ssp_ble.c"`), **not** a Modbus
  subsystem-bus TX as previously guessed.
- **Bike-state persistence to EEPROM.** `save_state_record_to_eeprom`
  (`0x0803E2CC`, named) CRCs the 56-byte state record `ctx[0x310..0x348]`
  through the hardware CRC unit, appends the 4-byte CRC, and writes the 60-byte
  record to an I2C **AT24C EEPROM** (slave `0xA0`), double-stored at EEPROM
  offsets 0 and 0x40. **Correction:** earlier docs called `0x0803E2CC` a
  "bike-state broadcast/apply"; it is the *persistence* path — so the
  `ble_cmd_dispatch` / `subsystem_update_sm` calls to it are "save the state
  record to EEPROM" after a change. Not sourced: it relies on a stack-aliasing
  ABI (only 4 reg args, but reads the caller-spilled 56-byte record), which C
  can't reproduce faithfully.
- **`ble_read_request_dispatch`** (`0x08034D20`, named, ~3.9 KB) — the GATT/SSP
  characteristic-**READ** dispatcher (the read twin of `ble_cmd_dispatch`'s
  writes): ~45 cases `0x5503..0x55C1` (+ motor-option `0x14`/`0x19`) gather a
  ctx field and reply via `ssp_ble_enqueue_tx_packet`; unknown ids log
  `"Unhandeled SSP request %04X"`. Several values use the `0xCCCCCCCD`/`>>35`
  divide-by-10 scaling.
- **`testmode_command_dispatch`** (`0x08029CA0`, named) — the "SH Set testmode"
  command (selector 0–12): selector 1 publishes a version/serial/HW telemetry
  batch over BLE/SSP svc `0x554A..0x5550`; selectors 2–0xC arm a 64-bit action
  bitmask at `ctx+0x3B8/0x3BC` (note: `0x3BC` is the error-flags word) and post
  event 0x18. Errors are only logged, never raised.
- **Leaves sourced:** `crc16` + `crc16_modbus_update` (`crc.c`, Modbus poly
  `0xA001` — completes the bus CRC); `flash_program_word` +
  `flash_unlock_and_clear_status` (`flash.c`, the FLASH HAL behind the
  staging); `ringbuf_get_byte` (`util.c`, the generic FIFO dequeue behind
  `ssp_rx_byte` and ~15 others); `amp_volume_brownout_apply` (`audio.c`,
  resolving the last `console.c` `FUN_080391B8` stub — supply-clamped volume +
  amp GPIO + bus TX); `maybe_set_state_if_unlocked` (`app.c`, FSM state byte
  setter, states 6/7 locked).

## Verbs, SSP transport completion & state accessors (workflow batch 2)

A second fan-out workflow analysed 12 functions (all returned, verified); 9
sourced, 3 named. Highlights:

- **BLE/SSP transport now complete end-to-end** (`ssp.c`). The four remaining
  handlers are sourced: `ble_data_packet`/`ble_prepare_packet` (the type-0x07 /
  0x06 thunks into `ble_cmd_dispatch` / `ble_read_request_dispatch`),
  `ble_command` (type-0x05 = an **ACK** that finds the queued TX packet whose
  sequence id matches, clears the slot, and runs its completion callback), and
  **`slip_send_frame`** (the SLIP **TX**: CRC-16 + `0xC0` framing + escaping —
  the counterpart of `slip_rx_packet`, and what `ble_ssp_dispatch` uses to send
  its ack). Full path: RX de-frame → dispatch by type → TX ack/notify.
- **FSM state byte get/set pair.** `maybe_get_bike_state` (`0x08029BA0`) reads
  the byte at SRAM `0x2000002D`; `maybe_set_state_if_unlocked` (`0x08029B88`,
  batch 1) writes it unless it's 6/7. So `0x2000002D` is the bike's lock/power
  state code (the value `alarm_state_name` stringifies).
- **`config_persist_dual_bank`** (`0x08031728`, named — *correction* of the
  earlier "config-apply to subsystem"): after a volume/region change it
  **persists the config to two redundant internal-flash banks** (primary
  `0x08008000`, backup `0x0800C000`) via a flash erase+write+CRC-verify worker,
  OR-ing the two status codes. Named only (stack-aliasing ABI like
  `save_state_record_to_eeprom`).
- Other leaves sourced: `log_print_timestamp_prefix` (`"%02d/%02d:%02d:%02d "`
  date/time prefix → `log.c`), `supply_voltage_read` (raw ADC → fixed-point →
  10-sample moving average → `sensor.c`), `maybe_set_pending_request` and
  `channel_notify_with_status` (→ `app.c`). Named-only: `enter_mode3_with_periodic_task`
  (a mode-3 + periodic-task state transition) and `flash_addr_to_sector`.

## Extern-completers & service steps (workflow batch 3)

A third fan-out analysed 14 functions (13 returned, 10 leaf-source); the focus
was closing out `extern FUN_*` references the recent modules left dangling.
5 sourced, 2 recognised as vendor-stock, 6 named+documented (with plate
comments in Ghidra). Highlights:

- **The image's allocator is plain newlib.** `FUN_08020E40` = `malloc`,
  `FUN_08020E50` = `free` (both tail into `_malloc_r`/`_free_r` on the
  `_impure_ptr` reentrancy struct at `0x2000011C`). So in `ssp.c` the TX path
  is now honest: `ssp_ble_enqueue_tx_packet` **`malloc`s** the payload copy and
  `ble_command` (the type-0x05 ACK) **`free`s** it — there was no "completion
  callback", just a `free` (batch-2 wording corrected).
- **`uart_send_byte`** (`0x080364F0`, → `uart.c`, new module). The serial TX
  primitive `slip_send_frame` calls per byte: masks the device TX interrupt
  (clears bit 7 of `dev+0xC`, DSB/ISB), pushes the byte into the TX ring via
  `ringbuf_push_byte` (`0x08031874`), re-enables the interrupt. **ABI quirk
  preserved:** it returns no value of its own — `r0` survives from the ring
  push, so the function implicitly returns the push status (1 queued / 0 full),
  which is exactly what `slip_send_frame` maps to its TX-full error. Verified by
  cross-reading `slip_send_frame`'s `if (uart_send_byte(0xC0) == 0)` tail.
- **`moving_avg10_push`** (`0x08032AB0`, → `sensor.c`) — the smoothing stage of
  `supply_voltage_read`. *Shares the ADC context struct at `0x20000914`*: write
  cursor (byte) at `+0x00`, ten `u16` samples at `+0x04`, then the ADC status
  byte `+0x22` and raw sample `+0x2A` used by `supply_voltage_read`. (The
  adversarial verifier caught a 1-vs-3 padding-byte layout error — samples land
  at `+0x04`, not `+0x02`.) Division by 10 via the `0xCCCCCCCD` reciprocal idiom.
- **`channel_resolve_status`** (`0x0802A2B0`, named→**sourced** in `app.c`):
  the helper behind `channel_notify_with_status`. Resolves a channel id against
  three priority bitmasks in the app context (`*0x20000944` + `0xF4/0xF8/0xFC`),
  returning 1..3 by priority or 0.
- **`state_flag_get`/`state_flag_set`** (`0x08036B8C`/`0x08036B80`, → `app.c`):
  the byte at SRAM `0x20000083` that `log_print_timestamp_prefix` brackets
  (save/zero/restore) — confirmed a plain state/mode flag, not an IRQ mask.
- Named+documented (hint corrections in **bold** were caught by the agents):
  `flash_config_bank_write` (`0x080316D0`, the single-bank worker under
  `config_persist_dual_bank`: erase → CRC-in-record → write `0xD0` → CRC-verify;
  stack-aliasing ABI, name-only); **`mainware_boot_init_sequence`**
  (`0x0803FC94` — *not* a flash routine: the ~1.5 KB top-level startup
  orchestrator — version print, GPIO/flash bring-up, per-subsystem init with a
  fault-retry/recovery loop, state-record validation, dual-bank config persist);
  **`sms_info_tracking_state_machine`** (`0x0803CC6C` — *not* an SSP service: the
  modem **SMS info-tracking** scheduler, part of the anti-theft path);
  `modbus_bat_service_step` (`0x0803F338` — the **battery (BMS) Modbus** link
  service step, distinct from BLE/SSP; retry/flush on the `0x80000` region tag);
  **`maybe_enqueue_tx_message`** (`0x0803A1C4` — *not* a per-characteristic GATT
  send: a generic enqueue into a second 16×24-byte TX table at `0x20007E14`,
  gated on link-connected); `rtc_fill_time_fields` (`0x080380A4`, STM32 HAL
  `GetTime`/`GetDate` → buf `[0]=hr [1]=min [2]=sec [0x16]=day`).

## Module-completing leaves & cloud builders (workflow batch 4)

A fourth fan-out analysed 14 functions (all returned, 13 leaf/thunk); the goal
was to *complete* the flash/ssp/crc/util modules and source the cloud builders.
9 sourced, 4 recognised as stock CubeF4 HAL, 1 thunk left provisional.

- **`flash.c` is now self-contained.** `flash_addr_to_sector` (the F413 1 MB
  sector map, unsigned-underflow range tests) is sourced. The three HAL leaves
  it/`flash_write` lean on are recognised as **stock CubeF4** and marked
  vendor-stock: `HAL_FLASH_Program` (`0x08027BE0`), `HAL_FLASH_Unlock`
  (`0x08027B14`), and — *correcting the earlier `lock_acquire` misnomer* —
  **`FLASH_WaitForLastOperation`** (`0x08027B80`). The latter fix makes
  `flash_write` read correctly: it waits for the prior flash op (timeout
  `0xFFFF`) before programming, it does not "acquire a lock".
- **`ssp.c` RX/TX primitives sourced.** `ssp_rx_byte` (atomic RX-ring pop, RX
  IRQ masked via a **double** pointer hop `*(*0x20009864)+0xC`) — the agent's
  first pass dropped one deref; the verifier caught it. Same implicit-return
  ABI quirk as `uart_send_byte` (returns the ring-get status through `r0`), which
  is what `slip_rx_packet`'s `== 0` test reads. `ssp_ble_seq_id_in_use` scans all
  128 TX slots' seq byte.
- **`util.c` ring is complete** — `ringbuf_push_byte` is the enqueue twin of
  `ringbuf_get_byte` (advance `head`, wrap at `cap`, bump `count`).
- **`crc.c`** — `crc32_hw_feed` (`0x0802320E`) streams words into the STM32 CRC
  peripheral via a driver handle at SRAM `0x20009D90`; `HAL_CRC_Accumulate`
  (`0x08023234`, the old `crc_stream_words`) is its stock-HAL sibling → vendor.
  So `0x20009D90` is the **CRC HAL handle**, refining the batch-3
  "config_crc_state" note for `flash_config_bank_write`.
- **`net.c`** (new module) — `bikecomm_request_build` / `ublox_request_build`:
  `snprintf` the cloud/modem HTTP request strings. Resolved the fixed flash
  strings: `bikecomm.vanmoof.com` (`0x08050890`), `ublox1.vanmoof.com`
  (`0x080508C0`), and the u-blox auth token `"PBNjh0V46Eev8CcfS4LPJg"`
  (`0x080508D4`, passed as the 3rd `%s`). These feed the modem AT+UHTTPC uplink.
- **`log_buffer_crc_check`** (`0x0802968C`, *was* mislabelled `config_crc_check`)
  → `log.c`: CRC-16 over the 8-byte circular-log-buffer header at SRAM
  `0x20037000`; on mismatch resets the buffer and writes
  `"Log cleared because invalid CRC"`. Named its two helpers `log_buffer_reset`
  / `log_emit_string`.
- **`channel_notify_emit`** (`0x0802A064`) → `app.c`: the sourced body behind
  `channel_notify_with_status`. For the four sound channel codes (5/0x1B/0x1C/
  0x1D) while the context mode byte (`*0x20000944+0x310`) is `0x0B` it runs a
  sound/clocking scheduler sub-path (force-set state `0x3D`, alloc+arm a slot for
  a code-specific duration), then always: amp-volume apply (brownout-limited),
  log `"SOUND_S%c vol %d"`, and enqueue a 2-byte BLE notify `{code,1}` as SSP
  command `0xC8`.
- *Provisional:* `0x080314D8` (`watchdog_kick`) is really a thunk to
  `FUN_08026DB4(desc@0x20009728)` (store `desc[0xC]` to `*desc[0]`); the
  watchdog reading rests on the call-site pattern, not confirmed bytes — name
  kept with a caveat.

## OTA validator, GPIO map, EEPROM & app leaves (workflow batch 5)

A fifth fan-out analysed 12 functions (all returned, 10 leaf/thunk). 9 sourced,
2 recognised as stock CubeF4 HAL, 1 deferred. New modules `gpio.c` + `eeprom.c`.

- **`pack_validate`** (`0x0803CFD8`) → `flash.c` — **the OTA image validator**.
  PACK magic is **`0xAA55AA55`** (the verifier corrected the agent's byte-swapped
  `0x55AA55AA`), length < `0x40000`. It resets the STM32 CRC unit, then hardware-
  CRC-32s the 10-word header (with the stored-CRC + length words masked to
  `0xFFFFFFFF`) immediately followed by the body — one continuous CRC, no reset
  between — and compares vs `pack[2]`. The verifier also caught an extra-deref on
  the CRC-handle arg (the struct base `0x20009D90` is passed directly, not its
  contents). Returns 0 ok / 1 CRC fail / 2 bad-magic-or-length.
- **`gpio.c`** (new) — `gpio_init` (`0x080314E8`): the **mainware pin map**. RCC
  AHB1 clock enables (E/H/C/A/B/D), initial output levels, then per-pin config
  via the now-recognised stock **`HAL_GPIO_Init`** (`0x080267D0`, vendor). Pin
  masks/levels folded into hardware.md.
- **`eeprom.c`** (new) — `eeprom_write_region` (`0x0803E258`): page-split
  (<=8-byte) writes to the AT24C I2C EEPROM (dev `0xA0`) via **`HAL_I2C_Mem_Write`**
  (`0x08024D2C`), 5 ms + watchdog between pages; address `offset | (page<<3)`.
- **`log.c` buffer ops** — `log_buffer_reset` (`0x0802957C`, zero the 100 KB
  payload + revalidate header CRC + console "Log cleared") and `log_emit_string`
  (`0x0802963C`, append with oldest-line eviction on overflow). Confirmed
  **`0x20009D98` is a single function pointer** (`g_log_func`, one deref + call) —
  validating the project-wide `log_func_t` model. Shared helper
  `log_buffer_header_crc_update` (`0x08029564`) named.
- **`app.c` mode/clocking leaves** — `aux_mode_byte_get`/`set_mode_state_byte`
  (`0x0802F0F8`/`0x0802E7F4`, the byte `0x20000068` that `channel_notify_emit`
  reads/writes — its callees now resolved); `enter_mode3_arm_show_timer`
  (`0x0802F104`, *was* `enter_mode3_with_periodic_task` — the `0x20000288`
  display-field-vs-`g_announce` overlap is now understood: byte+4 = display
  sub-field); and **`clock_pulse_gpioa8_until_pc9`** (`0x0803C8F4`) — the
  "Clocking %d" value is a **GPIO bit-bang**: pulse PA8 (3 ms/3 ms) up to 200×
  until PC9 goes high, returning the pulse count (a hardware clock/calibration probe).
- **Deferred:** `alarm_state_name` (`0x08032DF0`, renamed from `lock_state_name`)
  is a `state→name` table; only cases 0–3 (`ALARM_PRE_M1`, `ALARM_ACTIVE_M1_CNT`,
  `ALARM_ACTIVE_M2`, `ALARM_TRACKING_UNCONFIRMED`) + default `UNKNOWN` were
  resolved. Sourcing the other 58 with placeholder text would emit wrong log
  strings, so it stays named until the strings are read.
- Also recognised vendor: **`HAL_FLASHEx_Erase`** (`0x080235B4`, the
  `flash_erase` sector-erase callee) and **`HAL_GPIO_ReadPin`** (`0x08026AB8`).

## Module completion & provisional resolution (workflow batch 6)

A sixth fan-out analysed 12 functions (all returned, 10 leaf/thunk, all
verified faithful). 10 sourced, 2 vendor. Two new modules `i2c.c` + `watchdog.c`.
This batch closed out the externs the recent modules were leaning on.

- **`log.c` is self-contained** — the four SRAM circular-log-buffer primitives are
  sourced: `log_putc`/`log_getc` (cursor advance + wrap at the `0x18C00` end
  marker; write cursor @ header+4, read cursor @ header+0), `log_drain_line`
  (pop a line, oldest-first), and `log_buffer_header_crc_update` (CRC-16 over the
  8-byte header). `strlen` is recognised newlib (vendor). So the full log path —
  `log_emit_string` → `log_putc`/eviction → header CRC — is now real C.
- **`watchdog_kick` resolved** (`0x080314D8` → `watchdog.c`): the provisional
  thunk is confirmed a **window-watchdog refresh** — `wdg_reg_write_from_desc`
  (`0x08026DB4`) writes `desc[3]=0x7F` to `*desc[0]=WWDG_CR` (`0x40002C00`).
- **`clock_pulse_gpioa8_until_pc9` reframed as I2C3 bus recovery.** Its two
  callees are `i2c3_handle_deinit` (`0x0803C8E4`, thunk → `HAL_I2C_DeInit`) and
  `i2c3_handle_init` (`0x0803C660`, sets Instance=I2C3 `0x40005C00`, 100 kHz) →
  new `i2c.c`. So PA8 = SCL bit-bang, PC9 = SDA sense, and the routine de-inits
  I2C3, clocks SCL until SDA releases, then re-inits — the classic stuck-bus
  recovery. The I2C3 handle `0x20009B04` is the **same handle the EEPROM uses**.
- **`enter_mode3` callees resolved**: `scheduler_set_timer_name` (`0x08029B70`,
  a `bx lr` stub in this release build — the debug timer-name register) →
  `scheduler.c`; `reset_dual_buffers_and_flags` (`0x0803B780`, zeroes the two
  0x96-byte buffers in `g_request_ctx` @ `0x20008230`) → `app.c`.
- **`update_mode_get`** (`0x080313D8`) reads SRAM `0x20000076` — the **same byte
  as `g_update_mode`**; callers (e.g. `maybe_enqueue_tx_message`) treat value 2
  (idle) as "link ready", so it's the getter twin of `update_mode_request`.
- Confirmed **`0x20009D98` is a single function pointer** once more (the log
  primitives call it directly), and `0x20037000` is the 12-byte log header +
  ~100 KB payload to `0x2004FC00`.

## Bike state-name table (batch 7, resolved direct-from-binary)

The batch-7 *workflow* itself didn't land (its agents explored but never emitted
the structured result on the harder capstone targets, then the Ghidra MCP server
went unresponsive). But `alarm_state_name` (`0x08032DF0`) was resolved **directly
from the OEM image** by parsing its Thumb `tbh` jump table + literal pool +
strings — and sourced to `src/states.c` (62 cases + `UNKNOWN`; the Ghidra
rename/prototype were already done in batch 5, so no MCP was needed). Cases 0–3
match the batch-5-verified values exactly, confirming the parse.

These are the bike's **top-level state codes** (what `maybe_get_bike_state`
tracks and `status_process` logs), and they map the whole behavioural surface:
- **alarm / anti-theft:** `ALARM_PRE_M1`, `ALARM_ACTIVE_M1_CNT`, `ALARM_ACTIVE_M2`,
  `ALARM_TRACKING_UNCONFIRMED`/`_CONFIRMED`, `ALARM_BMS_REMOVED`, `ALARM_DELAY_ON`;
- **shipping / power:** `SET_SHIPPING`/`SHIPPING`, `BIKE_SHIPPING_ACCIDENTAL_WAKE`,
  `BIKE_SHIPPING_LIPOCHARGE`, `STANDBY`, `CPU_STOP_MODE`/`CPU_STOPPED`,
  `AUTOWAKEUP`, `LIPOCHARGE`, `CHARGING`, `LOW_SOC`, `TURN_ON`, `RIDING_MODE`;
- **OTA (OAD):** `OAD_UPDATE`, `OAD_FILE_TRF`, `OAD_RX_SOUND`, `OAD_FINISH`,
  `OAD_FAILED` — the BLE firmware-update state sequence;
- **PIN / lock:** `PIN_START`/`STUCK`/`1ST`/`2ND`/`3ND`/`CHECK`/`OK`/`SHOW_OK`/`NOK`/
  `NOK_SHOW`, `UNLOCK`, `EXTRA_ALREADY_UNLOCKED`, `UNLOCK_COUNT`(`_TIMEOUT`),
  `LOCK_PLAY_*`, `LOCK_DIM_OFF`, `LOCK_CLEAR`, `LOCK_SETUP`, `LOCK_PIC`, `LOCK_COUNT`;
- **misc:** `INIT`, `RESET`, `DIAGNOSE`, `DIAG_RDY`, `FACTORY_TEST`, `SHOW_LOCK`,
  `CARDRIDGE_REMOVED` (sic), `PLAY_FIRE`, `PLAY_SHTDN`(`_RDY`), `PLAY_LOCK_SHTDN`,
  `PLAY_LOCK_FROM_SLEEP`, and **`FIND_MY_PLAY`** (Apple Find My).

(The OEM overlaps some strings in flash — e.g. `SHIPPING` is the tail of
`SET_SHIPPING` — which the behaviour-equivalent C reproduces as plain literals.)

## Debug console command surface (batch 8 + table extraction)

The UART debug console (login-gated by `login_handler`) dispatches typed tokens
through a **49-entry command table at flash `0x0804F5C4`** — 12-byte
`{name, help, handler}` entries, extracted verbatim from the OEM image and mapped
in **`docs/console.md`**. A fan-out batch then sourced 11 of the richest handlers
into `console.c` (12 analysed, all leaf; `show` named+documented). **19/49
handlers are now decoded.**

- **Raw Modbus injection (the diagnostic bus surface).** `bwritereg`/`breadreg`
  hit the **battery/BMS** (Modbus slave **`0xAA`**); `swritedata`/`sreadreg` hit
  the **shifter** (slave **`0x20`**). Function codes: `0x03` read-holding-registers,
  `0x06` write-single-register, `0x10` write-multiple-registers. They build a raw
  frame and push it via `modbus_bat_submit` (`0x08039DDC`) / `modbus_shift_submit`
  (`0x080378A0`). (`swritedata` carries an OEM latent overflow — no length bound —
  preserved faithfully.)
- **Modem (tracking).** `gsminfo` dumps the cached u-blox identity — manufacturer/
  model/fw/imei/imsi/iccid/csq from the modem-info block at `*(ctx+0x3E8)` (16-byte
  text fields) plus the BLE MAC at `ctx+0x390`. `gsmstart` restarts the SMS-info
  state machine (zeroes its step byte `0x200000E5`). Three commands open sub-shells
  by UART redirect: `bledebug`→UART8 (CC2642), `gsmdebug`→UART2 (modem AT), `bmsdebug`→Modbus.
- **Config / modes.** `distance` (ctx+0x31C, tenths-km), `wheelsize` (ctx+0x10B,
  24/28-inch + dual-bank persist), `speed` (ctx+0x3C4/+0x3C6 override), `shipping`
  (bike state → 7), and `factory-shipping` (`0x08041FF8`) — the full powerdown:
  drop the GPIO power rails, deinit subsystems, BLE notify (cmd `0x112`) + Modbus
  cmd `0x14` draining each TX queue, then `shipping_powerdown_deinit(6)` (no return).
- **`show`** (`0x08042714`, named) is the `Parameters` dump — ~62 log lines that
  map most of the `ctx_sub` (session_ctx) struct: I/O pins, **Error Flags** at
  `ctx+0x3BC`/`+0x3B8`, Ibat `+0x3B2`, volumes `+0x105/6/7`, region `+0x109`, wheel
  `+0x10B`, light mode `+0x10C`, alarm/lock state `+0x310..+0x318`, distance `+0x31C`,
  modem info `+0x3E8`, BLE MAC `+0x390`, powerbank S/N+ver `+0x3D5..`, SOC `+0x3D4`.
- **Corrections the table forced:** the **volume** handlers are `vollow`/`volmid`/
  `volhigh` → ctx `+0x105`/`+0x106`/`+0x107` (the `app_state.h` struct + the two
  handler names were off by one; fixed, and the missing `volhigh` sourced). There
  is **no `soc` command** — the console **`gear`** command runs `console_soc_set`
  (writes `+0x3D4`, prints "Set SOC %d"): a firmware relabel, now documented.
- **Verifier catch:** several agents modelled the app-context base `0x20009368` as a
  pointer (one deref too many); the correct model is the existing `g_app_state`
  (struct base, `ctx_sub` pointer at `+0x2DC`) — all handlers integrated that way.

### Batch 9 — status / log / mode handlers (→ 33/49 decoded)

A second console fan-out sourced 13 more handlers into `console.c` (`ver` is
named+documented). The first lean-prompt run failed entirely (all 14 agents
explored but never emitted the structured result — the recurring StructuredOutput
flakiness); a tightened, tool-call-first re-run succeeded.

- **Subsystem telemetry refresh + scheduled dump pattern.** `battery`, `shifterstatus`,
  and `motorstatus` each clear a shadow region in `ctx_sub`, issue Modbus reads, and
  arm a periodic scheduler task whose callback prints the populated telemetry:
  `battery` → BMS shadow `ctx+0x3F2` (300 B) + two reads (regs 0x00–0x30, 0x47–0x56)
  + 1 s `console_battery_dump`; `shifterstatus` → 24 V rail (GPIOB p14) + HW-version
  poll (`ctx+0x520`); `motorstatus` → clears `ctx+0x364..0x376`, 500 ms poll + four
  motor Modbus requests (cmd 0x0C/0x0A/0x0B/0x0D).
- **ADC rails (`adc`)** — `hw_version_lookup` + `supply_voltage_read` (Vbat),
  `adc_read_vgsm`, `adc_read_5vsw`; all scale raw·3300/4096 mV off the ADC sample
  buffer at `0x20000914`. **`stc`** reads the battery STC fuel-gauge (`stc_read`) →
  "%d mV, %d mA %d%dC %d.%d%%".
- **Log control** — `logprn` dumps the circular buffer (`log_buffer_dump`) bracketed
  by suppressing live logging via `state_flag_set(0/1)` (so `0x20000083` is the
  **live-log-suppress flag**); `logclr` wipes it only with the confirmation key `6`.
- **Modes/system** — `batware` (start batteryware FW update: state 0x19 +
  `batteryware_update_arm` sets `*(0x20008A00+6)=4`), `factory` (`settings_factory_reset`),
  `reboot` (deferred `reboot_restart_task` after 600 ticks), `sound`
  (`channel_notify_with_status(idx)`), and the last Modbus injectors `bwritedata`
  (battery FC 0x10) / `swritereg` (shifter FC 0x06).
- `ver` (named) is the version/identity dump (app fw `0x08020004`, boot `0x08007FDC`,
  model string `ctx+0x64A`, serial/MAC `ctx+0x390`).

### Batch 10 — the remaining handlers (→ 49/49 decoded)

The final console fan-out sourced the last 16 handlers, **completing the entire
49-command surface**. Highlights:

- **Sub-shell openers.** `bledebug`/`gsmdebug` set a console UART-redirect selector
  byte (`ctx+0x34C` / `ctx+0x34D`) so the main loop bridges the console to UART8
  (the CC2642 BLE chip) / UART2 (the u-blox modem); `shiftware` uses the global
  selector at `0x2000019C` (=2). `bmsdebug`/`shiftdebug` are *not* redirects — they
  toggle Modbus debug-print flags (`ctx+0x34F` / `ctx+0x34E`).
- **`help`** walks the 49-entry dispatch table at `0x0804F5C4` printing each
  `{name, help}` (confirming the table layout). **`logout`** clears the session
  `logged_in` byte (`ctx+0x2D9`).
- **Hardware resets.** `blereset` pulses GPIOE PE5; `batreset` pulses GPIOB PB5 and
  schedules a 5 s release callback (which frees its own scheduler slot). `stcreset`
  resets the battery gas-gauge.
- **`setoad`** requests OAD (firmware-update) bike state `0x19`; **`setgear`** writes
  a gear-encoder position to the MT shifter (`ctx+0x520==0x201`) via a Modbus
  write-multiple. Plus the toggles/timers `loop`, `logapp` (log→APP, persists the
  state record), `powerchange`, `shiftresetcounter`.
- *Verifier catch:* `batreset`'s callback `FUN_080307A8(0x20000112)` is
  **`scheduler_release`** (freeing its one-shot slot), not "battery bring-up".

### Scheduled callbacks + `console.c` readability pass

- **Scheduled callbacks decoded.** The status commands install periodic scheduler
  tasks; these are now documented (field maps in their Ghidra plate comments):
  `console_battery_dump` (`0x0804175C`, ~50-field BMS telemetry, ctx+0x3F2..0x49E,
  with the −0xAAB Kelvin→Celsius temp offset), `motor_get_timer_cb` (`0x08040DE0`,
  motor telemetry ctx+0x364..0x388), `shifterstatus_dump_v200`/`_v201`
  (`0x080410C4`/`0x08040EEC`, the HW-0x200/0x201 shifter dumps incl. the per-gear
  position loop), and `reboot_restart_task` (`0x08038A68`, the multi-state
  shutdown/power-cycle SM — clears the warm-boot marker, bike state 0x16, BLE
  notify 0x11D, gas-gauge + factory reset, GPIO power sequencing). The trivial
  `shiftdebug_pump_task` (watchdog-kick self-rearm) is **sourced** into `console.c`.
- **`console.c` made human-readable.** Replaced the raw magic in the
  batch-8/9/10 handlers with named constants: the `ctx_sub` byte view `CTXB`,
  `CTX_*` field-offset macros (distance/wheelsize/speed/redirect/debug/loop/…),
  `MB_SLAVE_BAT`/`MB_SLAVE_SHIFT` + `MB_FUNC_*` Modbus codes, `GPIO*_BASE`, and named
  scheduler-slot / redirect-selector globals (78 offset renames + the bases).
- **Consistency fix surfaced by the refactor:** those handlers had hardcoded the
  OEM app-context address `0x20009368`, while the early handlers use the
  linker-placed `g_app_state` (a normal `extern`, *not* pinned). Routing every
  handler through `CTXB == g_app_state.ctx_sub` unifies them on the same object —
  a behaviour-equivalence correction (verified: only those handlers' literal pools
  changed, `0x20009368` → `&g_app_state`; the early handlers are untouched).

## Functions

### Decoded

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x080232E0` |  14 | `systick_tick`              | `src/systick.c` | `g_systick_counter += g_systick_step`; counter at SRAM `0x20009704`, step at SRAM `0x20000014` (shared `.data` offset with mainboot) |
| `0x080232F8` |   6 | `systick_now`               | `src/systick.c` | `return g_systick_counter` (ms since boot); byte-identical to OEM |
| `0x08023304` |  34 | `systick_delay`             | `src/systick.c` | busy-wait `ticks` (+`g_systick_step`) periods; `0xFFFFFFFF` = forever |
| `0x080424A4` | 236 | `volume_low_set`            | `src/console.c` | `vollow` cmd: show/set `ctx_sub->volume_low` (`+0x105`); range `[0,64]`, drives amp, config-persist |
| `0x080423B8` | 236 | `volume_medium_set`         | `src/console.c` | `volmid` cmd: writes `ctx_sub->volume_medium` (`+0x106`) |
| `0x080422CC` | 236 | `volume_high_set`           | `src/console.c` | `volhigh` cmd: writes `ctx_sub->volume_high` (`+0x107`) |
| `0x08041360` | ~70 | `console_cmd_distance`      | `src/console.c` | `distance` cmd: set ctx+0x31C (tenths-km), echo "Set %u.%u Km" |
| `0x08042120` | ~140 | `console_cmd_wheelsize`    | `src/console.c` | `wheelsize` cmd: ctx+0x10B (24→0/28→1) + dual-bank config persist |
| `0x0804131C` | ~60 | `console_cmd_speed`        | `src/console.c` | `speed` cmd: write override to ctx+0x3C4/+0x3C6 |
| `0x080415D0` | 18 | `console_cmd_shipping`      | `src/console.c` | `shipping` cmd: request bike state → 7 (created as a function this pass) |
| `0x08041FF8` | ~250 | `console_cmd_factory_shipping` | `src/console.c` | `factory-shipping`: GPIO rails off, BLE 0x112 + Modbus 0x14, powerdown (no return) |
| `0x08040D14` | ~170 | `console_cmd_gsminfo`      | `src/console.c` | `gsminfo` cmd: dump u-blox identity (*(ctx+0x3E8)) + BLE MAC (ctx+0x390) |
| `0x08041D38` | 16 | `console_cmd_gsmstart`      | `src/console.c` | `gsmstart` cmd: "Start GSM" + zero SMS-info step `0x200000E5` |
| `0x08041C84` | ~120 | `console_cmd_bwritereg`   | `src/console.c` | `bwritereg`: Modbus bat (0xAA) write-single-reg (func 0x06) via `modbus_bat_submit` |
| `0x08041B30` | ~110 | `console_cmd_breadreg`    | `src/console.c` | `breadreg`: Modbus bat (0xAA) read-holding-reg (func 0x03) |
| `0x0804168C` | ~190 | `console_cmd_swritedata`  | `src/console.c` | `swritedata`: Modbus shift (0x20) write-multiple (func 0x10) via `modbus_shift_submit` |
| `0x080414A4` | ~130 | `console_cmd_sreadreg`    | `src/console.c` | `sreadreg`: Modbus shift (0x20) read-holding-reg (func 0x03) |
| `0x08042F28` | ~110 | `console_cmd_battery`    | `src/console.c` | `battery`: wipe BMS shadow (ctx+0x3F2), 2 BMS reads, arm 1 s telemetry dump |
| `0x08042F74` | ~140 | `console_cmd_shifterstatus` | `src/console.c` | `shifterstatus`: 24V rail (GPIOB p14), clear ctx+0x51E, HW-ver status poll |
| `0x08042E54` | ~180 | `console_cmd_motorstatus` | `src/console.c` | `motorstatus`: 500ms poll, clear ctx+0x364.., 4 motor Modbus reqs (0x0C/0A/0B/0D) |
| `0x08043028` | ~120 | `console_cmd_adc`        | `src/console.c` | `adc`: HW version + Vbat/Vgsm/5Vsw rail mV (raw·3300/4096) |
| `0x08041614` | ~110 | `console_cmd_stc`       | `src/console.c` | `stc`: battery STC fuel-gauge V/I/temp/% |
| `0x08041E50` |  ~30 | `console_cmd_batware`   | `src/console.c` | `batware`: start batteryware FW update (state 0x19 + arm) |
| `0x08041F88` |  ~30 | `console_cmd_logprn`    | `src/console.c` | `logprn`: dump circular log buffer, suppress live logging during |
| `0x08041F34` |  ~70 | `console_cmd_logclr`    | `src/console.c` | `logclr`: clear log buffer (requires key 6) |
| `0x08041E70` |  ~30 | `console_cmd_factory`   | `src/console.c` | `factory`: settings_factory_reset(cfg, 1) |
| `0x08041DA4` |  18 | `console_cmd_reboot`     | `src/console.c` | `reboot`: schedule deferred restart task (+600 ticks) |
| `0x08041D10` |  ~30 | `console_cmd_sound`     | `src/console.c` | `sound`: channel_notify_with_status(idx) |
| `0x08041BB4` | ~190 | `console_cmd_bwritedata` | `src/console.c` | `bwritedata`: Modbus bat (0xAA) write-multiple (func 0x10) |
| `0x08041528` | ~130 | `console_cmd_swritereg` | `src/console.c` | `swritereg`: Modbus shift (0x20) write-single (func 0x06) |
| `0x08040AA0` | ~40 | `console_cmd_help`      | `src/console.c` | `help`: walk the 49-entry dispatch table, print name + help |
| `0x08040A4C` |  ~8 | `console_cmd_logout`    | `src/console.c` | `logout`: clear session logged_in byte (ctx+0x2D9) |
| `0x08041FB8` | ~30 | `console_cmd_blereset`  | `src/console.c` | `blereset`: pulse BLE reset (GPIOE PE5) high 10ms→low |
| `0x08040C6C` | ~30 | `console_cmd_bledebug`  | `src/console.c` | `bledebug`: UART8 sub-shell — set redirect ctx+0x34C |
| `0x08040C28` | ~60 | `console_cmd_loop`      | `src/console.c` | `loop`: print super-loop timing (ctx+0x358/35C/360), reset accum |
| `0x08041E94` | ~120 | `console_cmd_logapp`   | `src/console.c` | `logapp`: log→APP flag (ctx+0x313) + persist state record to EEPROM |
| `0x080412BC` | ~50 | `console_cmd_powerchange` | `src/console.c` | `powerchange`: set/report "ride change" flag (ctx+0x3CB) |
| `0x08041DD8` | ~60 | `console_cmd_batreset`  | `src/console.c` | `batreset`: pulse BMS reset (GPIOB PB5), 5s release callback |
| `0x08041DBC` | ~30 | `console_cmd_shiftware` | `src/console.c` | `shiftware`: start shifter FW update (redirect sel 0x2000019C=2) |
| `0x08041D50` | ~60 | `console_cmd_shiftdebug` | `src/console.c` | `shiftdebug`: toggle Modbus-shifter debug (ctx+0x34E) + pump task |
| `0x08040CB4` | ~22 | `console_cmd_shiftresetcounter` | `src/console.c` | `shiftresetcounter`: zero ctx+0x338 |
| `0x08040C90` | ~30 | `console_cmd_gsmdebug`  | `src/console.c` | `gsmdebug`: UART2 (modem) sub-shell — set redirect ctx+0x34D |
| `0x08040CD8` | ~38 | `console_cmd_bmsdebug`  | `src/console.c` | `bmsdebug`: toggle Modbus-BMS debug (ctx+0x34F) |
| `0x080415EC` | ~28 | `console_cmd_stcreset`  | `src/console.c` | `stcreset`: reset battery gas-gauge (gas_gauge_reset) |
| `0x080415B4` | ~30 | `console_cmd_setoad`    | `src/console.c` | `setoad`: request OAD update mode (bike state 0x19) |
| `0x080413B4` | ~190 | `console_cmd_setgear`  | `src/console.c` | `setgear`: write MT gear position via Modbus shift (func 0x10) |
| `0x08042590` |  28 | `console_start_motor_update`| `src/console.c` | logs `"Start motor update.."` + `FUN_080313E4(4)` |
| `0x080425AC` |  72 | `console_soc_set`           | `src/console.c` | parses int arg, writes `ctx_sub->set_soc` (`+0x3D4`), prints `"Set SOC %d"`, announces via `FUN_0802F1C0(2)` |
| `0x080425F4` | 166 | `login_handler`             | `src/console.c` | ES3 debug-console login callback; matches input against `g_app_state.ctx_sub->user_password` then hard-coded fallback at `0x080547EC`; 5-strike → 5 s scheduler-driven lockout |
| `0x08040A5C` |  66 | `console_next_token`        | `src/console.c` | line tokenizer; delimiters space/`.`/`:`, terminator `\0`; advances `*pp` to next token, returns 1 if one exists |
| `0x080421CC` | 220 | `console_region_set`        | `src/console.c` | `region` command: set region/speed mode `ctx[0x109]` (0=EU/1=US/2=JP/3=OFFROAD), gated on `ctx[0x144]`<3; re-applies config via FUN_08031728 |
| `0x0803F5A4` | 240 | `slip_rx_packet`           | `src/ssp.c` | SLIP de-framer + CRC-16 for the BLE/SSP byte stream; returns 0=frame ready, 2=error, 1=receiving |
| `0x0803F8FC` | 440 | `ble_ssp_dispatch`         | `src/ssp.c` | pull one SSP frame, dispatch by type (0x05 cmd / 0x06 prepare / 0x07 data→ble_cmd_dispatch); ack |
| `0x0803CF30` |  98 | `flash_erase`              | `src/flash.c` | erase shadow-flash sectors over `[addr,addr+len)` via `flash_erase_sector` |
| `0x0803CF94` |  64 | `flash_write`             | `src/flash.c` | IRQ-off word-program loop (`flash_program` op 2) into shadow flash; OEM IRQ-on-error quirk preserved |
| `0x0803C2A8` |  32 | `crc16_modbus_update`     | `src/crc.c` | per-byte CRC-16 step, reflected poly 0xA001 (byte-faithful) |
| `0x0803C2C8` |  18 | `crc16`                   | `src/crc.c` | CRC-16 over (buf,len,init), loops `crc16_modbus_update` |
| `0x08027A04` |  29 | `flash_program_word`      | `src/flash.c` | STM32F4 FLASH PG: set PSIZE=x32 + PG, store triggers word program |
| `0x0803CF1C` |  14 | `flash_unlock_and_clear_status` | `src/flash.c` | KEY1/KEY2 unlock + clear FLASH_SR (the eraser prep) |
| `0x080318AE` |  62 | `ringbuf_get_byte`        | `src/util.c` | generic byte-FIFO dequeue; behind `ssp_rx_byte` + ~15 others |
| `0x0803F9CC` | 172 | `ssp_ble_enqueue_tx_packet` | `src/ssp.c` | BLE/SSP TX queue enqueue (128×12-byte descriptors, rolling seq); ret slot/0xFD/0xFF |
| `0x080391B8` | 122 | `amp_volume_brownout_apply` | `src/audio.c` | supply-clamped amp volume apply (PD5/PD13 GPIO + bus TX cmd 0x96); resolved `console.c` stub |
| `0x08029B88` |  18 | `maybe_set_state_if_unlocked` | `src/app.c` | set FSM state byte (SRAM 0x2000002D) unless it's 6/7 (locked) |
| `0x08029BA0` |  10 | `maybe_get_bike_state`    | `src/app.c` | read the FSM state byte (SRAM 0x2000002D); getter twin of the above |
| `0x0803B738` |  16 | `maybe_set_pending_request` | `src/app.c` | latch a u32 request value + pending flag (SRAM 0x20008230) |
| `0x0802A2F0` |  18 | `channel_notify_with_status` | `src/app.c` | resolve a channel's status (0..3) and forward to the notify emitter |
| `0x0803F498` | ~70 | `ble_command`             | `src/ssp.c` | type-0x05 ACK: release the queued TX packet with matching seq id + `free` its heap payload |
| `0x0803F6A4` |   8 | `ble_data_packet`         | `src/ssp.c` | type-0x07 thunk → `ble_cmd_dispatch` |
| `0x0803F6AC` |   8 | `ble_prepare_packet`      | `src/ssp.c` | type-0x06 thunk → `ble_read_request_dispatch` |
| `0x0803F4F0` | ~90 | `slip_send_frame`         | `src/ssp.c` | SLIP TX: CRC-16 + `0xC0` framing/escaping; the TX twin of `slip_rx_packet` |
| `0x0803DBC8` | ~60 | `log_print_timestamp_prefix` | `src/log.c` | emit `"DD/HH:MM:SS "` via g_log_func, bracketed by a flag save/restore |
| `0x08032D6C` |  45 | `supply_voltage_read`     | `src/sensor.c` | raw ADC → fixed-point scale → 10-sample moving average |
| `0x08032AB0` |  62 | `moving_avg10_push`       | `src/sensor.c` | 10-sample circular moving average; shares the ADC ctx struct (`0x20000914`: cursor `+0`, samples `+4`); `/10` via `0xCCCCCCCD` |
| `0x080364F0` |  46 | `uart_send_byte`          | `src/uart.c` | serial TX primitive: mask TX IRQ (DSB/ISB) → `ringbuf_push_byte` → re-enable; implicitly returns ring push status (ABI quirk) |
| `0x0802A2B0` |  62 | `channel_resolve_status`  | `src/app.c` | resolve channel id vs 3 priority bitmasks (`*0x20000944`+`0xF4/F8/FC`); ret 1..3 by priority or 0 |
| `0x08036B8C` |  ~8 | `state_flag_get`          | `src/app.c` | read state/mode flag byte at SRAM `0x20000083` |
| `0x08036B80` |  ~8 | `state_flag_set`          | `src/app.c` | write state/mode flag byte at SRAM `0x20000083` (bracketed by `log_print_timestamp_prefix`) |
| `0x0802A064` | ~430 | `channel_notify_emit`    | `src/app.c` | sound/clocking scheduler sub-path (codes 5/1B/1C/1D) + amp volume apply + `"SOUND_S%c vol %d"` + 2-byte BLE notify (SSP cmd 0xC8) |
| `0x08031874` |  62 | `ringbuf_push_byte`       | `src/util.c` | generic byte-FIFO enqueue; push twin of `ringbuf_get_byte` (advance head, wrap at cap, bump count) |
| `0x08036528` |  34 | `ssp_rx_byte`             | `src/ssp.c` | atomic bus-RX-ring pop (RX IRQ masked via `*(*0x20009864)+0xC`); implicitly returns ring-get status (ABI quirk) |
| `0x0803F470` |  ~40 | `ssp_ble_seq_id_in_use`  | `src/ssp.c` | scan all 128 TX-queue slots' seq byte (+3) for `seq`; ret 1/0 |
| `0x0803CE14` |  ~90 | `flash_addr_to_sector`   | `src/flash.c` | F413 flash address → erase-sector 0..15 (unsigned-underflow range tests; 16K×4 / 64K / 128K…) |
| `0x0802968C` |  34 | `log_buffer_crc_check`    | `src/log.c` | CRC-16 the 8-byte log-buffer header at SRAM `0x20037000`; reset + `"Log cleared…"` on mismatch (was `config_crc_check`) |
| `0x0802320E` |  ~40 | `crc32_hw_feed`          | `src/crc.c` | feed N words into the STM32 CRC peripheral via the HAL handle @`0x20009D90`; return accumulated CRC |
| `0x0802F8A0` |  ~50 | `bikecomm_request_build` | `src/net.c` | `snprintf` request embedding `bikecomm.vanmoof.com`; ret 0 ok / 3 error-or-truncated |
| `0x0802F940` |  ~60 | `ublox_request_build`    | `src/net.c` | `snprintf` request embedding `ublox1.vanmoof.com` (×2) + token `PBNjh0V46Eev8CcfS4LPJg`; ret 0/3 |
| `0x0803CFD8` | 144 | `pack_validate`          | `src/flash.c` | OTA PACK validator: magic `0xAA55AA55`, len < 0x40000, HW CRC-32 (header crc/len masked, then body, continuous) vs `pack[2]`; 0/1/2 |
| `0x080314E8` | ~430 | `gpio_init`             | `src/gpio.c` | board GPIO bring-up: RCC AHB1 clock enables (E/H/C/A/B/D), initial levels, per-pin `HAL_GPIO_Init`. The mainware pin map |
| `0x0803E258` | ~110 | `eeprom_write_region`  | `src/eeprom.c` | AT24C I2C EEPROM page-split (<=8B) write via `HAL_I2C_Mem_Write` (dev 0xA0); 5 ms + watchdog per page |
| `0x0802957C` |  ~70 | `log_buffer_reset`      | `src/log.c` | reset SRAM log buffer: cursors→payload start, revalidate header CRC, zero 100 KB payload, console "Log cleared" |
| `0x0802963C` |  ~80 | `log_emit_string`       | `src/log.c` | append a string into the SRAM log ring (oldest-line eviction on overflow); ret length or 0xFFFFFFFF if >= 0x100 |
| `0x0802F0F8` |  ~8 | `aux_mode_byte_get`      | `src/app.c` | read the mode/sub-mode byte at SRAM `0x20000068` |
| `0x0802E7F4` |  ~8 | `set_mode_state_byte`    | `src/app.c` | write the mode/sub-mode byte at SRAM `0x20000068` (the `channel_notify_emit` clocking sub-mode) |
| `0x0802F104` | ~90 | `enter_mode3_arm_show_timer` | `src/app.c` | enter display mode 3: latch prior mode to display field, lazily alloc+arm the "ssp_show_tmr" 4000-tick task |
| `0x0803C8F4` | ~110 | `clock_pulse_gpioa8_until_pc9` | `src/app.c` | I2C3 bus recovery: deinit I2C3, pulse SCL (PA8) until SDA (PC9) high (max 200×), re-init I2C3; ret pulse count ("Clocking %d") |
| `0x080295D8` |  34 | `log_putc`                | `src/log.c` | write a byte at the log write cursor (header+4), advance + wrap at `0x18C00` end, set overflow flag |
| `0x080295B4` |  ~30 | `log_getc`               | `src/log.c` | read a byte at the log read cursor (header+0), advance + wrap |
| `0x08029604` |  ~50 | `log_drain_line`         | `src/log.c` | pop one line (oldest-first) from the log ring; copy to out if non-NULL; re-stamp header CRC |
| `0x08029564` |  ~30 | `log_buffer_header_crc_update` | `src/log.c` | CRC-16 over the 8-byte log header → header+8 |
| `0x0803B780` |  ~40 | `reset_dual_buffers_and_flags` | `src/app.c` | zero the two 0x96-byte buffers in `g_request_ctx` (`0x20008230`+0x01/+0x99), set +0x132, clear +0x130/1 |
| `0x080313D8` |  ~8 | `update_mode_get`         | `src/app.c` | read the update-mode/link-ready byte at SRAM `0x20000076` (2 = idle/ready) |
| `0x0803C8E4` |  ~12 | `i2c3_handle_deinit`     | `src/i2c.c` | thunk → `HAL_I2C_DeInit` on the I2C3 handle (`0x20009B04`); used in bus recovery |
| `0x0803C660` |  ~80 | `i2c3_handle_init`       | `src/i2c.c` | populate + `HAL_I2C_Init` the I2C3 handle (Instance I2C3 `0x40005C00`, 100 kHz, 7-bit) |
| `0x08029B70` |   2 | `scheduler_set_timer_name`| `src/scheduler.c` | `bx lr` stub (debug timer-name register), paired with `scheduler_start` |
| `0x080314D8` |  12 | `watchdog_kick`           | `src/watchdog.c` | refresh the WWDG: thunk → `wdg_reg_write_from_desc` (writes 0x7F to WWDG_CR) |
| `0x08026DB4` |  ~16 | `wdg_reg_write_from_desc`| `src/watchdog.c` | descriptor-driven 32-bit store: `*desc[0] = desc[3]` (desc@`0x20009728`: WWDG_CR ← 0x7F) |
| `0x08032DF0` | ~520 | `alarm_state_name`       | `src/states.c` | alarm/bike `state→name` table (62 cases, `tbh` jump table); all strings read from the OEM image — incl. OAD/PIN/lock/`FIND_MY_PLAY` |
| `0x08031444` | ~40 | `watchdog_init`          | `src/watchdog.c` | build the WWDG descriptor @`0x20009728` {WWDG_CR,0x180,0x7F,0x7F,0} + `wwdg_hw_init`; `Error_Handler` on failure |
| `0x08026D8A` | ~30 | `wwdg_hw_init`           | `src/watchdog.c` | HAL_WWDG_Init: clock-enable + `WWDG_CR=T|WDGA(0x80)`, `WWDG_CFR=W|WDGTB`; 0 ok / 1 if NULL |
| `0x08031474` | ~20 | `wwdg_clk_enable`        | `src/watchdog.c` | `RCC_APB1ENR |= WWDGEN` (bit 11) if the descriptor targets WWDG (`0x40002C00`) |
| `0x0803DDCC` | ~12 | `Error_Handler`         | `src/panic.c` | CubeF4 fatal handler: `g_log_func("Error_Handler\r\n")` + spin; shared by WWDG/I2C init failures |
| `0x080306D8` |  96 | `scheduler_tick`            | `src/scheduler.c` | SysTick dispatch over 48 armed slots; decrement counter (floored at 0), fire callback once when it lands on 1 |
| `0x0803073C` | 100 | `scheduler_alloc`           | `src/scheduler.c` | first-free slot from `allocated` bitmap (`+0x00`); zero counter+cb, set bit; on full table → `muco_assert_fail` (no return) |
| `0x080307A8` |  84 | `scheduler_release`         | `src/scheduler.c` | clear both `armed`(`+0x08`)+`allocated`(`+0x00`) bits, zero counter+cb, write `*slot_ref=0xFA`; ret 1/0 |
| `0x08030800` |  50 | `scheduler_start`           | `src/scheduler.c` | store counter=`ticks`, cb; set `armed` bit; re-arm just resets the counter |
| `0x08030838` |  26 | `scheduler_slot_is_idle`    | `src/scheduler.c` | `slot<48 && counters[slot]==0` |
| `0x080306C0` |  20 | `scheduler_init`           | `src/scheduler.c` | clear both slot bitmaps (all 48 free) at startup; returns 1 |
| `0x0803C974` |  12 | `NMI_Handler`              | `src/exceptions.c` | log `"NMI_Handler\r\n"`, return |
| `0x0803C988` |  18 | `HardFault_Handler`        | `src/exceptions.c` | naked; pick MSP/PSP via `tst lr,#4`, tail-call `fault_dump` (assembles byte-for-byte) |
| `0x0803C99C` |  12 | `MemManage_Handler`        | `src/exceptions.c` | log, spin (`b .`) |
| `0x0803C9B0` |  12 | `BusFault_Handler`         | `src/exceptions.c` | log, spin |
| `0x0803C9C4` |  12 | `UsageFault_Handler`       | `src/exceptions.c` | log, spin |
| `0x0803C9D8` |  12 | `SVC_Handler`             | `src/exceptions.c` | log, return |
| `0x0803C9EC` |  12 | `DebugMon_Handler`        | `src/exceptions.c` | log, return |
| `0x0803CA00` |  12 | `PendSV_Handler`          | `src/exceptions.c` | log, return |
| `0x0803CA14` |  12 | `SysTick_Handler`         | `src/exceptions.c` | `scheduler_tick()` then `systick_tick()` |
| `0x0803CB6C` | 166 | `fault_dump`              | `src/exceptions.c` | print stacked R0-R3/R12/LR/PC/xPSR + SCB MMFAR/BFAR/CFSR/HFSR/DFSR/AFSR, spin |
| `0x0803DAC4` |  16 | `muco_assert_fail`        | `src/panic.c`     | `g_log_func("FATAL error File [%s] line [%d]\r\n", file, line)`, spin; no-return |
| `0x080313E4` |  16 | `update_mode_request`     | `src/app.c`       | set subsystem update-mode byte (SRAM `0x20000076`) iff currently idle (==2) |
| `0x0802F1C0` |  28 | `announce_mark`           | `src/app.c`       | set broadcast dirty-flag for channel 0/1 (SRAM `0x2000028D`/`0x2000028E`); other channels no-op |
| `0x0802311C` |  18 | `bcd_to_bin`              | `src/util.c`      | packed-BCD byte → binary `(bcd&0xF)+(bcd>>4)*10` |
| `0x08022F2E` |  22 | `bin_to_bcd`              | `src/util.c`      | binary byte → packed BCD (count-tens loop) |
| `0x08043AA4` |  72 | `SystemInit`              | `src/system_stm32f413.c` | FPU enable + RCC reset + `VTOR=0x08000000`; OEM op-sequence match |

### Decomp-asm

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x08043E54` |  72 | `Reset_Handler` | `src/startup_stm32f413.S` | set SP, copy `.data`, zero `.bss`, paint `[_ebss,_estack)` with `0x0000000E`, then `SystemInit`→`__libc_init_array`→`main`. Asm (startup); co-located with the vector table + envelope + `Default_Handler`. |

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |
| `0x08020980` | 198 | `__floatsidf` | libgcc soft-float signed-int → double (`0x432` exponent bias, `0x100000` mantissa MSB). mainware does double math in software (the FPU is single-precision only). |
| `0x08020DEC` |   6 | `__getreent` | newlib reentrancy-pointer getter — returns `_impure_ptr` (`*0x2000011C`), the `_REENT` that `strtol` & friends use. |
| `0x08020EC0` | 348 | `memcpy` | newlib optimised `memcpy` — unaligned head fix-up, 16-word (64-byte) unrolled main loop, 4-word / word / half / byte tails; returns `dst`. Supplies the `ctx_sub[0x104..0x1C4]` snapshot in `volume_set_common`. |
| `0x08021428` | 730 | `strcmp` | newlib/glibc optimised C strcmp (byte fast-path + 4/8-byte aligned word compares using `uadd8`/`sel`). Returns `*s1 - *s2` of first differing byte. Will pick up from vendored newlib once that's wired in. |
| `0x08021EAC` |  18 | `strtol` | newlib reentrant trampoline: loads `_REENT` from `*0x2000011C` and tail-calls `_strtol_r` at `0x08021D5C`. Standard `long strtol(const char *s, char **endptr, int base)`. |
| `0x080212B0` | 136 | `snprintf` | newlib `snprintf` core (sets up the `_REENT` string-FILE, calls the `vfprintf` engine `0x0802210C`, NUL-terminates; `EOVERFLOW` on negative size). The cloud request builders format through it. |
| `0x08026AC6` |  12 | `HAL_GPIO_WritePin` | CubeF4 HAL inline: writes `pin_mask` to `GPIOx->BSRR` if state non-zero, otherwise `pin_mask << 16` (atomic bit-set / bit-reset). |
| `0x08020DF8` | 100 | `__libc_init_array` | newlib C-runtime init: runs `__preinit_array`, calls `_init`, runs `__init_array`. Called second by `Reset_Handler`. |
| `0x08043EBC` |   4 | `_init` | empty newlib/crti `_init` stub (`bx lr`), called by `__libc_init_array`. |
| `0x08020E40` |  16 | `malloc` | newlib `malloc(size)` — loads `_impure_ptr` (`*0x2000011C`) and tail-calls `_malloc_r`. `ssp_ble_enqueue_tx_packet` allocates TX payload copies through it. |
| `0x08020E50` |  ~20 | `free` | newlib `free(ptr)` — loads `_impure_ptr` and tail-calls `_free_r`. `ble_command` frees the TX payload through it. |
| `0x08027BE0` | 116 | `HAL_FLASH_Program` | CubeF4 HAL: width-tagged program (`TypeProgram` 0/1/2/3 = byte/half/word/dword), `Data` is a u64. Reentrancy-guarded + locked. `flash_write` calls it (word). |
| `0x08027B14` |   ? | `HAL_FLASH_Unlock` | CubeF4 HAL: KEY1/KEY2 unlock of `FLASH_CR` (idempotent — checks LOCK first). Distinct from the muco `flash_unlock_and_clear_status`. |
| `0x08027B80` |   ? | `FLASH_WaitForLastOperation` | CubeF4 HAL: poll `FLASH_SR` BSY with a tick timeout + clear handle ErrorCode. *Was mislabelled `lock_acquire`* — `flash_write` waits-for-op before programming. |
| `0x08023234` |   ? | `HAL_CRC_Accumulate` | CubeF4 HAL: accumulate a word buffer through the CRC peripheral (sibling of the bespoke `crc32_hw_feed`). |
| `0x080267D0` |   ? | `HAL_GPIO_Init` | CubeF4 HAL: configure a port's pins (MODER/OTYPER/OSPEEDR/PUPDR/AFR) from a `GPIO_InitTypeDef`. Driven by `gpio_init` + `clock_pulse_gpioa8_until_pc9`. |
| `0x08026AB8` |   ? | `HAL_GPIO_ReadPin` | CubeF4 HAL: 1 if any masked `GPIOx->IDR` bit set, else 0. |
| `0x080235B4` |   ? | `HAL_FLASHEx_Erase` | CubeF4 HAL: sector/mass erase (`FLASH_EraseInitTypeDef` + `&SectorError`). The `flash_erase` sector callee. |
| `0x08024D2C` |   ? | `HAL_I2C_Mem_Write` | CubeF4 HAL: blocking I2C memory write (dev/mem-addr/size/timeout). Used by `eeprom_write_region`. |
| `0x08021740` |   ? | `strlen` | newlib `strlen` (word-at-a-time NUL scan). Used by `log_emit_string`. |
| `0x08024570` |   ? | `HAL_I2C_Init` | CubeF4 HAL: configure an I2C peripheral from the handle's Init fields. Used by `i2c3_handle_init`. |
| `0x0802472C` |   ? | `HAL_I2C_DeInit` | CubeF4 HAL: disable + reset an I2C peripheral handle. Used by `i2c3_handle_deinit`. |

### Named (no source yet)

| Address | Size | Name | Why named |
| --- | --- | --- | --- |
| `0x0803DEA8` | 613 | `main` | application entry / super-loop, called last by `Reset_Handler`. Structurally decoded (see "Application structure" above); body sourcing waits on its ~60 callees. Currently a weak spin-placeholder in `startup_stm32f413.S` so the tree links. |
| `0x08031900` | ~2.6K | `subsystem_update_sm` | whole-bike OTA firmware-update state machine (see "Subsystem firmware update" above). Deep — dozens of callees; mapped, not sourced. |
| `0x0802AAF8` | ~14K | `status_process` | the per-loop bike-status/event processor (`main` calls it each iteration); emits Diag + Error Flags. Too large for the decompiler — see "Diagnostics / error flags". |
| `0x08033970` | ~2.5K | `ble_cmd_dispatch` | the BLE app-command surface — ~40-case switch on the 16-bit command id (lock/region/power/LED/backup-code/…). **Full map in `docs/ble-commands.md`.** Mapped, not sourced. |
| `0x0803E2CC` |  65 | `save_state_record_to_eeprom` | CRC + write the 56-byte state record `ctx[0x310..0x348]` to I2C AT24C EEPROM (0xA0), double-stored at 0/0x40. Named only — stack-aliasing ABI. |
| `0x08029CA0` | 830 | `testmode_command_dispatch` | "SH Set testmode" cmd (sel 0–12); sel 1 = version/serial/HW telemetry over BLE svc 0x554A–0x5550; sel 2–C arm a 64-bit action mask. |
| `0x08034D20` | ~3.9K | `ble_read_request_dispatch` | GATT/SSP characteristic-READ dispatcher (~45 cases 0x5503–0x55C1); read twin of `ble_cmd_dispatch`. |
| `0x08031728` |  82 | `config_persist_dual_bank` | persist a config snapshot to two internal-flash banks (0x08008000 / 0x0800C000) via a flash erase+write+CRC worker; OR'd status. Stack-aliasing ABI — named only. |
| `0x080380A4` |   ? | `rtc_fill_time_fields` | fill a buffer with current RTC time fields via STM32 HAL GetTime/GetDate (`[0]=hr [1]=min [2]=sec [0x16]=day`). |
| `0x080316D0` |  82 | `flash_config_bank_write` | single-bank config writer under `config_persist_dual_bank`: erase → CRC-in-record → write `0xD0` → CRC-verify. Stack-aliasing ABI — named only. |
| `0x0803FC94` | ~1473 | `mainware_boot_init_sequence` | top-level startup orchestrator (version print, GPIO/flash bring-up, per-subsystem init + fault-retry/recovery, state-record validation, dual-bank config persist). *Hint "flash routine" corrected.* |
| `0x0803CC6C` |   ? | `sms_info_tracking_state_machine` | modem SMS info-tracking scheduler (4-state, anti-theft tracking path). *Hint "SSP service" corrected.* |
| `0x0803F338` |   ? | `modbus_bat_service_step` | per-loop battery (BMS) Modbus link service step (pump SM, retry/flush on `0x80000` region tag). Distinct from BLE/SSP. |
| `0x0803A1C4` |   ? | `maybe_enqueue_tx_message` | generic enqueue into a 16×24-byte TX table at `0x20007E14`, gated on link-connected. *Hint "GATT send" corrected.* |
| `0x0803DDE0` |   ? | `boot_init_cold` | `main`'s cold-boot path — taken when SRAM `0x20000000 != 0x55AA55CF`; precedes the full init sequence. |
| `0x0803DADC` |   ? | `boot_init_warm` | `main`'s warm-boot path — taken when the `0x55AA55CF` retained-RAM marker is present (skips cold init). Contains `muco_assert_fail` calls. |

### Pending decomp targets (next to look at)

**Deferred (the remaining capstone):**
- **`main` (`0x0803DEA8`) + `boot_init_cold`/`boot_init_warm`** — the super-loop
  capstone. Already **structurally mapped** in "Application structure" below; full
  C sourcing is deep-document (depends on the last ~dozen un-named loop callees)
  and may want address-pinning to be meaningful. Left named.

(The batch-7 WWDG-builder + I2C error-trap completers are now **done** —
`watchdog_init`/`wwdg_hw_init`/`wwdg_clk_enable` → `watchdog.c`, the shared
`Error_Handler` → `panic.c`; `watchdog.c` + `i2c.c` are fully self-contained.)

**Keystone — DONE.** `startup_stm32f413.S` (envelope + 128-entry vector table +
`Reset_Handler` + `Default_Handler`), `system_stm32f413.c` (`SystemInit`), and
`log.c` (the `g_log_func` global) now root the graph via `ENTRY(Reset_Handler)`
+ `KEEP(.isr_vector)`. The build links to a **real, non-empty image** (`make`
text ≈ 2 KB skeleton: envelope, vectors, the boot path, and the SysTick
closure). `make compare` is live again. Two pending callees that the OEM
`Reset_Handler` invokes are weak placeholders until sourced: `main` (decoded,
spins) and `__libc_init_array` (newlib, returns).

**Byte-match status (read before trusting `make compare`).** The 512-B
**envelope reproduces byte-perfectly** except 7 bytes — the CRC32 (`+0x08`) and
total length (`+0x0C`), both post-build patches. `SystemInit` compiles to the
OEM's exact operation sequence (even the two-`bic` split of `0xFEF6FFFF`).
*But* the overall image is **not** byte-identical: our decoded functions land at
linker-assigned addresses, not their OEM addresses, so the vector table values
and every `.text` body differ. A meaningful full-image diff (where the
*differing-byte count drops as decode coverage grows*) needs **address pinning**
— placing each decoded function at its OEM address in the linker script and
filling the gaps — i.e. a matching-decomp setup. That's a strategic build change
worth deciding deliberately; flagged here, not done. The FPU is enabled
(`SystemInit` sets `CPACR`), so a pinned/complete build will want
`-mfloat-abi=hard -mfpu=fpv4-sp-d16` once FP-using functions are decoded.

Console-handler callees still opaque (the remaining `extern FUN_*` stubs in
`console.c`):

| Address | Size | Notes |
| --- | --- | --- |
| `0x08031728` |  ? | Audio-engine apply (4-word arg). Called from `volume_*_set` after every volume change with the four words at `ctx_sub->audio_engine_cfg[0..3]`. |
| `0x080391B8` |  ? | Volume validator. Called with a pointer to the just-parsed volume byte; non-zero return triggers a `" ERR set volume"` log. |
| `0x080313E4` |  ? | Subsystem-mode request. Called as `(4)` from `console_start_motor_update`. |
| `0x0802F1C0` |  ? | Broadcast/announce. Called as `(2)` from `console_soc_set` after writing the SOC override. |

Full list in `ghidra/exports/mainware_program.json` (refresh via
`ghidra/scripts/DumpMainwareProgram.java`).

## Security findings

- **Hard-coded debug-console password in flash.** `login_handler`
  (`0x080425F4`) accepts a 40-character fallback `"vEVjGF!paYs
  M2EBV8SoDT8*T0eB&#T6xevaoxCaO"` stored verbatim in rodata at
  `0x080547EC`. Path-tracing confirms the fallback is accepted
  **regardless** of whether the user has set their own service
  password — `strcmp(input, "")` never returns 0 for a non-empty
  input (and empty inputs are filtered out earlier), so the
  user-password compare always falls through to the hard-coded one
  when the user-side slot is empty. The hand-written sanity guard
  `user_password[0] != '\0'` is dead-code under standard `strcmp`
  semantics. Worth checking whether `mainware_1.08.02.bin` and
  `mainware_1.09.*.bin` still ship the same constant. The 5-strike
  / 5-second lockout via the Muco scheduler is the only brute-force
  mitigation, and any input typed during cooldown re-arms the
  lockout to a fresh 5 s.

  **Persistence across versions.** Verified by `strings | grep`: the
  exact 40-character constant appears once in each of
  `mainware_1.07.06.bin` (Nov 2021), `mainware_1.08.02.bin`
  (May 2022), `mainware_1.09.01.bin` (May 2023), and
  `mainware_1.09.03.bin` (Jun 2023). The backdoor was shipped
  unchanged for ~19 months across the entire visible release
  history.

- **Hard-coded u-blox cloud auth token in flash.** `ublox_request_build`
  (`0x0802F940`) formats the modem HTTP request with the token
  `"PBNjh0V46Eev8CcfS4LPJg"` stored verbatim at `0x080508D4`, alongside the
  endpoint host `ublox1.vanmoof.com` (`0x080508C0`); `bikecomm_request_build`
  (`0x0802F8A0`) targets `bikecomm.vanmoof.com` (`0x08050890`). The token is a
  static credential the modem presents to the VanMoof backend — shared across
  the fleet (same firmware image), not device-derived. Worth checking whether
  it rotates across the 1.08/1.09 releases.

## Open questions

- Exact mainware flash slot — `0x08040000` is the working hypothesis
  from VTOR alignment; confirm from `mainboot`'s "Jump to App" code
  path.
- VTOR set by mainboot before jump — must equal the slot base
  `0x08040000` (or the per-slot equivalent) for the table at file
  offset `0x200` to dispatch correctly.
- FPU usage — does any function emit `vpush`/`vpop` (would require
  switching to `-mfloat-abi=hard -mfpu=fpv4-sp-d16`)?
- Modem command flow — is the uBlox SARA driver a clean state machine
  or a soup of inline `printf`+`expect` calls?
- BLE-side protocol — the bleware on the CC2642 talks to mainware over
  Modbus (same bus the shifter and motor use)? Or a separate UART /
  SPI link?
- What is at file offset `0x010..0x028` exactly — the ASCII build
  date+time looks like literal `__DATE__` + `__TIME__` placed at a
  known location by the linker script. Confirm by inspecting the
  early `.text` for a reference to `0x08040010`.
