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
| 130 | pending (auto-named `FUN_xxxxxxxx`) — live Ghidra count; **the bespoke application layer is now 100% sourced.** The remainder is **116 vendor/stock-HAL (newlib/libgcc + CubeF4) + 14 phantom-already-sourced** (Ghidra-split tails / externs whose OEM addr is cited in src). Verified by a 14-agent fan-out: **11/11 newly-decoded functions PASS** against the raw disassembly and a 3-range census critic **examined 137 `FUN_` and found 0 missed bespoke** ("could not falsify" the completeness claim). See "Remaining `FUN_` triage". |
| 24  | vendor-stock — `strcmp`, `strtol`, `strlen`, `snprintf`, `memcpy`, `memset`, `__libc_init_array`, `_init`, `__getreent`, `malloc`, `free` (newlib), `__floatsidf` (libgcc); CubeF4 HAL: `HAL_GPIO_WritePin`, `HAL_GPIO_Init`, `HAL_GPIO_ReadPin`, `HAL_FLASH_Program`, `HAL_FLASH_Unlock`, `HAL_FLASHEx_Erase`, `FLASH_WaitForLastOperation`, `HAL_CRC_Accumulate`; CMSIS `NVIC_DisableIRQ` (`0x080270FC`, writes `NVIC->ICER`); `memcmp`, `strstr`, `strchr`, `strncmp` (newlib — the last named `bounded_strncmp`, returns 0 == equal). (The CubeF4 **I²C** HAL — `HAL_I2C_Init`/`DeInit`/`Mem_Write`/`Mem_Read`/`Master_Transmit`/`Master_Receive` — is no longer vendor-stock: reconstructed as faithful C in `i2c.c`, see the per-module log.) |
| 0   | in-progress |
| 518 | decomp-c — `vectors.c` (25), `tim.c` (9), `stc3115.c` (16), `console_edit.c` (12), `systick.c` (3), `console.c` (65), `scheduler.c` (8), `exceptions.c` (10), `panic.c` (2), `app.c` (25), `util.c` (6), `system_stm32f413.c` (1), `ssp.c` (22), `motor.c` (9), `flash.c` (8), `crc.c` (6), `audio.c` (1), `log.c` (10), `sensor.c` (9), `uart.c` (8), `bus.c` (3), `net.c` (2), `gpio.c` (5), `eeprom.c` (3), `i2c.c` (21), `watchdog.c` (6), `states.c` (7), `modem.c` (56), `ble.c` (5), `ble_read.c` (3), `rtc.c` (9), `update.c` (1), `main.c` (4), `battery.c` (29), `display.c` (33), `lighting.c` (9), `display_requests.c` (data: 38 descriptors), `shifter.c` (39), `lis3dh.c` (26), `comm.c` (2) — see per-module log below |
| 1   | decomp-asm — `startup_stm32f413.S`: `Reset_Handler` (+ vector table, envelope, `Default_Handler`, `reset_region_empty_stub`) |
| 134 | named (rename in Ghidra, no source yet) — (`comm_buffers_register_all` moved named→sourced this pass) — the STC3115 **config/startup/algorithm layer** named+prototyped this pass (extern, bodies deferred): `stc3115_apply_config` (`0x080394DC`, writes CC_CNF/VM_CNF/alarm regs + MODE), `stc3115_startup_from_ocv` (`0x08039580`), `stc3115_startup_restore` (`0x080395A8`), `stc3115_init_device` (`0x080395D0`, top init: sets cfg params, checks RAM sig 0x53A9 + CRC, branches restore-vs-init); the SOC-tracking task `stc_read` (`0x080396E4`), `stc3115_fuel_gauge_init` (`0x08037130`) and `stc3115_wake` (`0x080398CE`) remain named — the **CubeF4 HAL TIM core** named+prototyped this pass (extern in `tim.c`, bodies deferred): `HAL_TIM_Base_Init` (`0x080277B0`), `HAL_TIM_PWM_Init` (`0x080277E4`), `TIM_Base_SetConfig` (`0x080276E8`), `HAL_TIM_ConfigClockSource` (`0x08026DC0`), `HAL_TIM_PWM_ConfigChannel` (`0x08027888`), `TIM_OC1/2/3/4_SetConfig` (`0x080273B4`/`0x08027818`/`0x0802741C`/`0x0802748C`), `HAL_TIMEx_ConfigBreakDeadTime` (`0x08026E48`), `TIM_CCxChannelCmd` (`0x08027964`), `HAL_TIM_PWM_DeInit` (`0x0802752A`); plus the global `HAL_MspInit` (`0x0803C270`, PWR+SYSCFG clock enable) and `i2c2_wait_bus_idle` (`0x0803C8A8`, I2C2 SR2.BUSY 50 ms wait used by the display/light I2C path) — the **9 shared IRQ leaves** the vector trampolines forward to, named this pass (extern in `vectors.c`, bodies decode-pending): `rtc_wakeup_irq_handler` (`0x08027020`), the CubeF4 HAL servicers `HAL_DMA_IRQHandler` (`0x08022BDC`), `HAL_TIM_IRQHandler` (`0x0802756C`), `HAL_I2C_EV_IRQHandler` (`0x08025E04`), `HAL_I2C_ER_IRQHandler` (`0x08025F9E`), and the four EXTI/TIM application pre-hooks `exti4_app_hook` (`0x08043CEC`), `exti9_5_app_hook` (`0x08038FF4`), `tim6_app_hook` (`0x08037AA8`), `tim7_app_hook` (`0x08039138`) — `system_reset` (`0x08035CE8`, AIRCR SYSRESETREQ) + `console_io_table_install` (`0x080430D8`, binds `g_log_func` to the UART7 console table) + `usart1_io_table_install` (`0x0804309C`, binds `g_log_func` to the USART1 console table) named as referenced leaves of the UART byte-pump cluster (bodies extern, not sourced; `uart_rx_ringbuf_get_byte` was *sourced*) — `HAL_GPIO_EXTI_Callback` (`0x08038964`, the application's EXTI dispatch override, named this pass; its ~150-byte body is fired by the now-sourced `HAL_GPIO_EXTI_IRQHandler` — left for a future pass) + the UART-init HAL externs `HAL_RCC_GetPCLK2Freq` (`0x08027394`), `HAL_RCC_GetHCLKFreq` (`0x08027368`), `HAL_UART_MspInit` (`0x080333C0`) named this pass (called from `uart.c`'s reconstructed init path; `HAL_UART_MspDeInit`/`HAL_RCC_GetPCLK1Freq` were already named) + `modem_sms_dispatch_command` (`0x0803D668`, the inbound-SMS remote-command interpreter, named this pass) + **24 named leaves were SOURCED into their home modules this pass** (see the "Named-leaf sourcing pass" log entry): the ble_read telemetry getters (`ble_get_charge_plug_state`/`ble_build_testmode_versions_blob`/`charge_level_adc_get`/`ble_get_led_channel_state`/`telemetry_map_clamp`/`gpio_pc0_is_low`/`gpio_pc1_is_low`/`hw_version_lookup`/`maybe_enqueue_tx_message`), the bike-state getters (`bike_status_coarse_get`/`bike_state_is_standby`/`ble_lock_state_get`/`ble_unlock_state_get`), the RTC helpers (`rtc_now_epoch_seconds`/`rtc_set_from_unix_time` → new `rtc.c`), app/state helpers (`app_ctx_clear_field_328`/`clear_flag_00e5`/`count_active_2bit_groups`/`announce_records_reset`/`display_announce_enter`/`display_timeout_timer_set`/`lock_poll_timer_arm`) and the BLE helpers (`sspm5_tx_timeout_cb`/`post_request_with_arg`). `config_persist_dual_bank` stays named (its OEM body memcpy's 0xC0 bytes from `&stack0x00000000` — a variadic stack passthrough that doesn't model cleanly in portable C). The rest still pending. — (the **LED-matrix display engine** is now **SOURCED → `display.c`** (`display_module_init`/`display_send_init_cmd`/`display_write_reg20_init`/`display_panel_reset`/`led_driver_panel_config`/`led_driver_brightness_write`/`led_driver_standby_write`/`led_driver_set·enter_shipping_mode`/`led_matrix_render·overlay_frame_region`/`led_matrix_transmit_step`/`matrix_draw_speed·number·icon·level_bar·level_bar_blink`/`matrix_set_corner_led·turn_indicator`/`matrix_glyph_src_addr·frame_delay`/`display_mode_sm_step`/`display_request_*`/`is_display_bus_ready`/etc.) and the **lamp engine + ambient sensor** → **`lighting.c`** (`light_pattern_step`/`light_pattern_action_apply`/`light_sensor_read_step·i2c_read·fault_count_get`/`obj_set_field34·38`/`led_channel3_set_brightness`), with **~25 FUN_ helpers named this pass**, fan-out transcribed + adversarially verified (`docs/display.md`, `docs/lighting.md`). Two name corrections: `dsp_recovery_telemetry_pump`→**`led_matrix_transmit_step`** ("dsp" in `" ERR dsp freeze"` = display, not the C28x DSP); and `light_tick_update` (`0x080371E8`) identified as the **command-mode console↔modem/BLE-debug UART bridge** (it shuttles bytes between the UART7 console and either USART2/GSM via `gsmdebug` or UART8/BLE-debug via `bledebug`), NOT lamps (still named, out of scope). — The battery/BMS Modbus driver — `modbus_bat_submit`/`bms_modbus_read`/`bms_modbus_write`/`bat_modbus_master_step`/`bms_telemetry_unpack`/`battery_telemetry_step`/`battery_state_process`/`battery_charge_display_step`/`modbus_bat_service_step`/etc. — is now **SOURCED → `battery.c`**; the shared bus CRC/TX helpers `bus_crc16_get`/`update`/`reset`/`verify` + `bus_tx_enqueue_byte`/`_n` were named this pass, extern pending a future `bus.c`. The spine `main`/`boot_init_cold`/`boot_init_warm`/`mainware_boot_init_sequence` **and `status_process`** are now **SOURCED** — `status_process` → `states.c` (the 62-case behaviour engine, adversarially verified), the rest → `main.c`; with their **~74 callees named this pass**, `docs/boot.md`: peripheral init `hal_mcu_init`/`dma_controller_init`/`usart1·2·3·6_init`/`uart5·5·7·8_init`/`i2c2_init`/`tim1_pwm_init`/`tim6·7·10_init`/`adc1_init`/`rtc_init`/`crc_init`/`comm_buffers_register_all`, clock `rcc_oscillator_config`/`rcc_clock_config`/`rcc_periph_clock_config`/`tim_channel_enable_output`, loop services `light_tick_update`/`light_pattern_step`/`modbus_shifter_link_monitor`/`lipo_charge_state_monitor`/`ssp_ble_tx_queue_pump`/`motor_fw_update_fsm_step`/`sspm_rx_reply_handler`/`sspm_tx_queue_pump`/`led_matrix_render·overlay_frame_region`/`dsp_recovery_telemetry_pump`/`charger_and_pc1_sense_debounce`/`supply_voltage_sample_step`/`output_value_filter_step`/`ble_telemetry_change_broadcast`/`update_sm_is_idle`/`log_upload_sm_step`/`display_mode_sm_step`/`factory_reset_sm_step`/`staged_msg_validate_and_dispatch`/`button_press_state_machines_step`/`app_ctx_ptr_set`, boot devices `display_module_init`/`hdc1080_write_config_reg`/`stc3115_wake`/`stc3115_fuel_gauge_init`/`lis3dh_accel_init`/`audio_amp_init`/`display_write_reg20_init`/`eeprom_read_id_block`/`eeprom_read_config_with_crc_fallback`/`flash_read_config_with_crc_restore`/`sound_groups_init_default`/`region_speed_preset_table_load`/`flash_program_rdp_level_once`/`reset_reason_log_and_clear`/`log_wake_reason`/`log_console_subsystem_init`), OTA helpers (`flash_cache_disable`, `flash_cache_enable`, `download_chunks_pending_count`, `shifter_update_status_get`, `shifter_update_request`, `batteryware_update_status_get`, `batteryware_update_set_pending`, `bus_rx_byte_locked`), BLE (`maybe_enqueue_tx_message`), lock/alarm state (`bike_is_locked`, `ble_lock_state_get`, `ble_unlock_state_get`, `bike_state_is_standby`, `bike_status_coarse_get`), modem/tracking (`modem_sim_state_machine`, `sms_info_tracking_state_machine`), battery (`modbus_bat_service_step`, `modbus_bat_submit`, `modbus_shift_submit`, `battery_request_telemetry`, `bms_modbus_read`, `console_battery_dump`, `stc_read`, `gas_gauge_reset`, `batteryware_update_arm`), motor (`motor_get_timer_cb`), shifter (`shifterstatus_dump_v200`, `shifterstatus_dump_v201`), ADC (`hw_version_lookup`, `adc_read_vgsm`, `adc_read_5vsw`), console (`console_cmd_show`, `console_cmd_ver`), log (`log_buffer_dump`), flash/eeprom (`config_persist_dual_bank`, `flash_config_bank_write`, `save_state_record_to_eeprom`, `settings_factory_reset`, `reboot_restart_task`, `bat_reset_release_cb`), misc (`testmode_command_dispatch`, `rtc_fill_time_fields`), **+ 42 `status_process` per-state sub-handlers** (`status-process.md`): shifter-SM steps (`shifter_sm_get_step`/`set_step_3`/`_10`/`_13`, `shifter_get_active_flag`, `shifter_firmware_update_step`), `state_flags_set`/`clear`/`test` (64-bit flag pair ctx+0x3B8), LIS3DH (`lis3dh_int1_clear`/`powerdown`/`config_motion_int` — **SOURCED → `lis3dh.c` this pass**; `accel_enable`→`stc_gas_gauge_set_run`), `locked_state_step`, `power_assist_gear_step`, `diagnostics_run_step`, `internal_lipo_charge_step`, `enter_stop_mode`, `system_reset` (NVIC), `led_driver_set`/`enter_shipping_mode`, `light_sensor_read_step`, `charge_level_adc_get`, `battery_on_detect_step`/`substate_advance`, `bms_write_reg8_and_poll`, `telemetry_datalog_emit`, `sched_timer_arm_or_alloc`, `set_unlock_state_persist`, `sms_track_state_get`, `state_table_ptr_get`, etc. |

`function_count = 814` per `ghidra/exports/mainware_program.json` (3 OEM functions newly
created an earlier pass: `console_cmd_shipping`, `shiftdebug_pump_task`, `bat_reset_release_cb`;
+ `motor_post_update_cb` `0x08030994` created in the motor-update FSM pass — the dump must
materialize it, so `function_count` is now 815. The **USART2/UART8/UART7 byte-pump pass**
**created** one more function — `console_printf` `0x080367F0` (the previously-undefined
g_log_func[0] printf) — so the dump must materialize it too, making it **816**. The **USART1
console-port pass** created `usart1_printf` `0x08035EBC` (the previously-undefined USART1 printf)
→ the dump must materialize it: `function_count` is now **817**).
The 49-command console dispatch table is mapped in `docs/console.md` — **all 49 handlers
decoded** (47 sourced into `console.c`, `show` + `ver` named+documented).
**The committed JSON is stale**: ~240 functions have been renamed + given
prototypes/no-return across the recent sessions — incl. the shifter pass (23
`FUN_*` renamed to `bus_queue_*`/`shifter_*`/`modbus_frame_flush` and the new
function `shift_rx_flush_timeout_cb` @ `0x08037410`, which the dump must
materialize) and the placeholder-decode pass (~45 leaves named, incl. the
corrections `accel_enable`→`stc_gas_gauge_set_run` and
`flash_cache_disable`/`enable`→`wwdg_apb_clk_disable`/`enable`) (everything in the Decoded,
Decomp-asm, Vendor-stock and Named tables below carries its OEM address, so
those tables are the authoritative name map until the JSON is regenerated).
The GhidraMCP server can't run the dump script — re-run
`ghidra/scripts/DumpMainwareProgram.java` in Ghidra to refresh it. The program
itself was saved after each session. (This pass renamed **36 more** functions —
the 32 modem AT callbacks `0x0802F1DC..0x0802FD68`, `strstr`/`strchr`/`bounded_strncmp`,
and `modem_sms_dispatch_command` — set 36 prototypes, and labelled the 13
`g_modem_*` SRAM scratch globals + `g_pModemProvision` @ `0x0804F440`.) The
**LIS3DH pass** renamed **25 more** (`0x0803D070/074/098` transport, `0x0802E434/444/44E`
thunks, the 15 register helpers `0x0802E45E..0x0802E782`, `lis3dh_int1_read_source`
`0x0803D1D4`, `charge_time_estimate_reset` `0x0802E40C`, `NVIC_DisableIRQ` `0x080270FC`,
`HAL_I2C_Mem_Read` `0x08024E90`), set **29 prototypes**, labelled `g_lis3dh_dev`
@ `0x2000838C` + `g_lis3dh_int1_last_src` @ `0x200001E0`, and added 2 plate comments —
program saved. The **I²C HAL pass** renamed **15 more** (the 10 transfer statics
`0x08023EEC..0x08024504`, `HAL_I2C_Master_Transmit` `0x08024760`,
`HAL_I2C_Master_Receive` `0x080248D8`, plus the 3 referenced externs
`HAL_RCC_GetPCLK1Freq` `0x08027374` / `HAL_I2C_MspInit` `0x0803C69C` /
`HAL_I2C_MspDeInit` `0x0803C820`) and set **12 prototypes** — program saved.
The **UART HAL init pass** renamed **6 more** (`HAL_UART_Init` `0x08026CFC`,
`UART_SetConfig` `0x08026AF0`, `HAL_UART_DeInit` `0x08026D5A` ← was
`uart_handle_deinit`, `HAL_RCC_GetPCLK2Freq` `0x08027394`,
`HAL_RCC_GetHCLKFreq` `0x08027368`, `HAL_UART_MspInit` `0x080333C0`) and set
**6 prototypes** — program saved.
The **GPIO HAL deinit/EXTI pass** renamed **3 more** (`HAL_GPIO_DeInit`
`0x08026990`, `HAL_GPIO_EXTI_IRQHandler` `0x08026AD4`, `HAL_GPIO_EXTI_Callback`
`0x08038964`) and set **3 prototypes** — program saved.
The **SSPM-bus TX pass** renamed **1 more** (`sspm_bus_send_byte` `0x0803662C` ←
was `FUN_0803662C`) and set **2 prototypes** (`sspm_bus_send_byte`,
`sspm_bus_send_frame` `0x0803A008`) — program saved.
The **motor-DSP download transport pass** renamed **5 more** (`sspm_bus_get_byte`
`0x08036664`, `motor_dl_send_buf_step` `0x08030B70`, `motor_dl_recv_buf_step`
`0x080309C4`, `motor_dl_recv_byte_step` `0x08030A50`, `motor_dl_recv_u16_step`
`0x08030ADC`) and set **5 prototypes** — program saved.
The **motor-DSP transaction-SM pass** renamed **3 more** (`motor_dl_send_verify_step`
`0x08030BFC`, `motor_dl_autobaud_step` `0x08030CEC`, `motor_dl_stream_block_step`
`0x08030D88`) and set **3 prototypes** — program saved.
The **motor-update FSM pass** sourced `motor_fw_update_fsm_step` `0x08030FF4` (was
already named), set its prototype, and **created + named a new function**
`motor_post_update_cb` `0x08030994` (the 30-byte one-shot scheduler callback — body
decode-pending) — program saved.
The **USART IRQ-handler pass** renamed **4 more** (`usart3_irq_handler` `0x080362D0`,
`uart5_irq_handler` `0x08036560`, `uart4_irq_handler` `0x08036424`,
`usart6_irq_handler` `0x0803669C` ← were `FUN_*`) and set **6 prototypes** (those 4
plus `bus_tx_enqueue_byte` `0x0803639C` / `bus_rx_byte_locked` `0x080363EC`,
re-typed to return `int`) — program saved. No new functions created, so
`function_count` stays 815.
The **USART2/UART8/UART7 byte-pump pass** renamed **14 more** `FUN_*` and **created +
named one new function** (`console_printf` `0x080367F0`): the three remaining serial
byte-transport triads — USART2/GSM modem (`modem_uart_putc` `0x080360F8`,
`modem_uart_flush` `0x08036130`, `modem_uart_rx_byte` `0x08036144`, `usart2_irq_handler`
`0x0803617C`), UART8/BLE-debug (`uart8_tx_byte` `0x08036A38`, `uart8_rx_byte`
`0x08036A70`, `uart8_irq_handler` `0x08036AA8`), and UART7/console (`uart7_tx_byte`
`0x08036754`, `uart7_puts` `0x0803678C`, `uart7_write` `0x0803679E`,
`uart_rx_ringbuf_get_byte` `0x080367B8`, `console_printf` `0x080367F0`,
`uart7_irq_handler` `0x080368D4`) — plus `ringbuf_free_space` `0x080318EC`, and the two
referenced leaves `system_reset` `0x08035CE8` / `console_io_table_install` `0x080430D8`.
**15 prototypes set**, 6 RAM globals labelled (`g_uart7_dev_pp` `0x200097E4`,
`g_uart8_dev_pp` `0x200098E4`, `g_gsm_dev_pp` `0x200099A4`, `g_console_uart_ctx`
`0x20003C34`, `g_console_state` `0x20004D2C`, `g_console_log_echo` `0x20000083`), program
saved. One new function created → `function_count` was then **816**.
The **USART1 console-port pass** renamed **6 more** `FUN_*` and **created + named one new
function** (`usart1_printf` `0x08035EBC`): the USART1 console byte transport — twin of the
UART7 block — `usart1_tx_byte` `0x08035E28`, `usart1_puts` `0x08035E5C`, `usart1_write`
`0x08035E6E`, `usart1_rx_byte` `0x08035E88`, `usart1_printf` `0x08035EBC`, `usart1_irq_handler`
`0x08035F98`, plus the referenced installer `usart1_io_table_install` `0x0804309C`. **7
prototypes set**, 2 RAM globals labelled (`g_usart1_dev_pp` `0x200098A4`, `g_console_port_sel`
`0x20000114`), program saved. One new function created → `function_count` is now **817**.

## Remaining `FUN_` triage (full sweep)

**Refreshed census (now re-confirmed by a 14-agent verification + 3-range completeness
critic, every remaining function decompiled): of the 130 `FUN_` still in the program,
exactly — 116 vendor/stock-HAL (NOT worth decoding, HAL boundary) + 14 phantom
already-sourced (Ghidra-split tails / externs whose OEM addr is cited in src). The 12
"genuinely-undecoded bespoke" carried below are now ALL SOURCED (this pass).** The census
critic examined 137 `FUN_` across 0x08020000–0x0805FFFF and found **0 missed bespoke** —
it positively re-ID'd the previously-fuzzy strays too (`0x080313F8`=newlib `_sbrk` guard;
`0x08032BB4`/`C60`/`C6A`=stock `HAL_SPI_MspInit`/`MspDeInit` for SPI1; `0x0803C3B6`=
`HAL_TIM_PWM_MspInit` tail). The decomp is **complete for bespoke VanMoof logic** — only
newlib/libgcc + CubeF4 HAL remain extern by design.

**The 14 phantom already-sourced (do NOT re-count as pending):** the 8 modem step-tails
`0x080301E6`–`0x08030642` (split loop-bodies of the sourced `modem_step_*`),
`tx_table_handle_in_use` (`0x08039FE0`, ssp.c), `console_activity_timer_rearm` (`0x08029FE8`,
console_edit.c), the `HAL_TIM_PWM_MspInit` TIM1 tail (`0x0803C3B6`, tim.c), and the HAL
boundaries `HAL_RTC_SetTime`/`SetDate` (`0x08022F44`/`0x08023042`, rtc.c) + the I2C2-DMA write
(`0x08024BC0`, display.c) — all kept extern by design.

**The genuinely-undecoded bespoke — ALL 12 NOW SOURCED (this pass, 11/11 adversarially verified PASS):**
- ~~HIGH `0x08043148`~~ **DONE → `console_passthrough_io_install` (console.c)** (prior pass).
- **`0x080317F4` → `comm_register_buffer` + `0x08035D0C` → `comm_buffers_register_all` (new `comm.c`)** —
  the 16-slot inter-module comm-buffer registry (pool @SRAM `0x2000069C`: init-flag byte + 16×0xC
  records {buf, size(u16), 3×u16 zeroed}); `comm_buffers_register_all` wires all 16 link buffers from
  the five per-port contexts (`0x2000094C`/`0x20004DB4`/`0x20001A44`/`0x20002B3C`/`0x20003C34`).
- **`0x0803805C` → `rtc_msp_init` + `0x08038A04` → `rtc_wakeup_event_cb` (rtc.c)** — `HAL_RTC_MspInit`
  (enables `RCC_BDCR.RTCEN` via the bit-band alias `0x42470E3C` + NVIC IRQ 3 = RTC_WKUP) and the
  wake-up event latch (sets `*(*(u32**)(0x20000094+0x24))=1`).
- **`0x08040288` → `HAL_CRC_MspInit` + `0x080402B8` → `HAL_CRC_MspDeInit` + `0x080402E8` →
  `crc_accumulate_device_uid` (crc.c)** — CRC clock-gate on/off (`RCC_AHB1ENR` bit 12, CRCEN, base
  `0x40023830`) and a CRC-32 over the 96-bit device unique-ID (`0x1FFF7A10`) used by `console_cmd_show`.
- **`0x0803A538` → `ble_interval_debounce` (ble.c)** — the BLE telemetry-change 8000-tick one-shot
  debounce (`scheduler_*`, timer name `"*tmr"`), used by `ble_telemetry_change_broadcast`.
- **`0x08032CBC` → `adc_config_shadow_copy` (sensor.c)** — latches the live ADC sampling config
  (`ADC_CTX+0x18/0x1c/0x20`) into the shadow (`+0x24/0x28/0x2c`) while status byte `+0x22` is clear.
- **`0x0803D648` → `sms_tracking_latch_once` + `0x0803D65C` → `sms_tracking_get` (modem.c)** — the SMS
  info-tracking one-shot latch + getter (flag @`0x2000839C`).
- **`0x08043EB8` = `Default_Handler`** (already reconstructed in `startup_stm32f413.S` — the spin target
  of the unused vector slots; renamed in Ghidra this pass, not a new function) **and `0x08043EC8` →
  `reset_region_empty_stub`** (an unreferenced no-op frame in the reset region; reproduced in startup.S).

(Historical 4-range survey below, retained for the vendor/HAL detail.)

**Not worth decoding (~130, vendor/stock — decode only for a byte-exact build):**
- **newlib / libgcc (~30):** the `0x08020400–0x0802290C` block — `vfprintf`/`_printf_i`/pad,
  `_malloc_r`/`_free_r`/`_realloc_r`/`_sbrk_r`, `__aeabi_dmul`/`dadd`/`d2iz`/`uldivmod`,

**Not worth decoding (~130, vendor/stock — decode only for a byte-exact build):**
- **newlib / libgcc (~30):** the `0x08020400–0x0802290C` block — `vfprintf`/`_printf_i`/pad,
  `_malloc_r`/`_free_r`/`_realloc_r`/`_sbrk_r`, `__aeabi_dmul`/`dadd`/`d2iz`/`uldivmod`,
  `memchr`/`memmove`/`memmem`/`strnlen`.
- **Stock CubeF4 HAL (~85):** **CAN** bxCAN (9), **RTC** Init/SetTime/SetDate (6), **FLASH**
  option-byte/RDP + program byte/half/word + unlock/lock/erase (16), **GPIO** AFR/MODER/EXTI (6),
  **UART** SetConfig + full IT engine (14), **I2C** master/mem/DMA IRQ interior (16), **TIM**
  Start/Stop + IRQ interior stubs (6), **RCC**/MCO/clock-freq (3), **CRC**/SysTick (3), and ~15
  empty `HAL_*_Callback` weak stubs.

**Worth decoding — bespoke app (~45), by cluster (priority order):**
1. ~~**Modem AT-command state machine (HIGH, ~11):**~~ **DONE / was over-counted.** The eight
   "AT-sequence runners" `0x080301E6`/`08030286`/`08030326`/`080303C6`/`08030466`/`08030502`/
   `080305A2`/`08030642` are **Ghidra-split tails** of the already-sourced `modem_step_sms_init` /
   `_sms_read` / `_sms_write` / `_ctx_activate` / `_ctx_deactivate` / `_ping_send` / `_message_send` /
   `_location_send` in `modem.c` (each parent has a conditional branch Ghidra promoted to a separate
   function — they are NOT undecoded). `0x0802F3A0` was the already-sourced `modem_at_response_match`
   (renamed in Ghidra). `0x08035C94` (the RX response-line accumulator, misnamed `modem_uart_tx_byte`)
   was the only genuinely-unsourced piece → **now sourced in `modem.c`**. `0x080330E4` is a
   status→string mapper belonging to cluster #4 (announce dispatcher), not modem. Net: modem layer
   complete; the 8 tail-fragments remain as cosmetic `FUN_` artifacts of sourced parents.
2. **Debug-console command engine + VT100 editor (HIGH, ~13):** **11/13 DONE → new `console_edit.c`**
   (10/10 adversarially verified): `console_cmd_match` (`0x08040904`), `console_history_init`/`_rotate`
   (`0804094C`/`080409A0`), the two dispatchers `console_dispatch_login` (`08040A00`, pre-auth: runs the
   `login` cmd) / `console_dispatch_command` (`080426BC`, logged-in match+arg-split), history nav
   `console_history_recall`/`_prev`/`_next` (`08041184`/`080411CA`/`080411FA`), tab-autocomplete
   `console_autocomplete_apply`/`_search` (`08041232`/`0804125C`), and the VT100/CSI→keycode decoder
   `console_vt100_decode_key` (`080431A4`), and **now the line editor itself** `console_line_editor`
   (`080434F8`, ~200 lines — insert/overwrite, backspace/delete with mid-line redraw, Home/End/cursor
   moves, history up/down, tab-completion echo, Enter→dispatch; 3 adversarial-verify catches folded in:
   the Delete reposition loop's `+1`, the Tab `write(ac_match,ac_len)` source, and the pre-login
   "recogni**s**ed" British-spelling string vs the logged-in "recogni**z**ed"). **One non-editor stray
   deferred:** `FUN_08043148` — not a prompt-setup but a 3rd console-I/O-table installer (writes
   g_log_func[0..4] by port selector) whose installed [0]/[2]/[3] point at undefined functions
   `0x08036B74/76/78`; resolve those first.
3. ~~SSPM inter-module bus (HIGH)~~ **DONE → `ssp.c`** — `sspm_bus_recv_frame` (SLIP+CRC16 RX
   de-framer), `tx_table_release_by_handle`/`tx_table_free_count` (`08039F90`/`0803A510`;
   `08039FE0` was already `tx_table_handle_in_use`), `sspm_ble_cmd_bridge`/`sspm_ble_read_bridge`.
4. ~~Announce/telemetry dispatcher (HIGH)~~ **DONE → `battery.c`** — `manchester_announce_decode`
   (serial/fw/SoC/SoH·NoC·WST), `wst_status_to_string`, `staged_msg_crc16`, `tim10_announce_period_cb`
   (TIM10 double-buffer). Verify caught the SoC `%d` being signed (OEM `ldrsb`) — fixed.
5. ~~HDC1080 driver (MED)~~ **DONE → `sensor.c`** — `hdc1080_set_pointer` + `hdc1080_read`
   (soft-float, 0.1 °C / %RH; constants 2⁻¹⁶/165/40/10/100, signed temp / unsigned RH).
6. ~~EXTI/TIM filters (MED)~~ **DONE** — `sensor_deglitch_filter` (6-sample min/max-reject mean,
   `0x08038ED4`) → `sensor.c`; `console_magic_sequence_match` (`"\x1b[14~"` gesture ring,
   `0x08037188`) → `console.c`. (The named app-hooks `exti9_5_app_hook`/`tim7_app_hook` that *call*
   the filter, plus `0x08029FE8`/`0x0803A538` timer glue, remain bodies-deferred.)
7. ~~I2C event callbacks + bus scan (MED)~~ **DONE → `i2c.c`** — `i2c_tx_complete_callback`,
   `i2c_error_callback` (+`display_request_recovery`), `i2c_bus_scan` (diagnostic 0..0xFE probe).
8. **Low/glue:** misc setters/getters/flag-latches (`0x0803D648`/`0803D65C`, `08040288`/`080402B8`
   bit-toggles, `08032CBC` shadow-reg copy, MspInit/DeInit strays `08032BB4`/`08032C60`/`08032C6A`/
   `0803805C`/`0803C3B6` — the last is the `HAL_TIM_PWM_MspInit` fall-through fragment).

## Per-module decomp log

- **3 deferred named-externs SOURCED (the validation sweep's leftovers) + 6 more validated — all PASS.**
  The three functions earlier flagged as named-only (stack-aliasing/intricate) are now reconstructed and
  **adversarially verified 3/3 PASS** (build clean **text 1996**, 0 warnings): **`modem_sms_dispatch_command`
  → modem.c** (`0x0803D668`, the inbound-SMS remote-command interpreter — `#<8-char code>*<cmd>` →
  `key`/`TrackingOn`/`TrackingOff`/`ping`/`makeNoise`; the SMS half of the anti-theft surface paralleling
  `ble_cmd_dispatch`. All command + log + format strings resolved byte-for-byte from rodata; the `ping` JSON
  status report does signed `/10` on battery (`int16 ctx+0x3D2`) and distance (`int32 ctx+0x31C`), reads the
  firmware version from flash `0x08020004`, and formats the MAC from `ctx+0x390..0x395`). **`flash_config_bank_write`
  → flash.c** (`0x080316D0`, the single-bank config writer behind `config_persist_dual_bank`: erase → CRC the
  first 0x33 words into the record's reserved last word → program 0xD0 bytes → self-checking CRC verify from
  flash; modeled with a `union{w[0x34]; {hdr[4];boot_cfg_block;}}` for the by-value record + indexed CRC store).
  **`save_state_record_to_eeprom` → app.c** (`0x0803E2CC`, the dual-copy EEPROM state writer: CRC first 0xE
  words into word 0xE → write at EEPROM offsets 0 and 0x40 with a 5 ms/watchdog gap; same union pattern,
  link-compatible with all ~19 existing callers which each keep their own extern). **6 more validated PASS:**
  `eeprom_write_region`, `console_soc_set`, `modem_step_message_send`, `modem_at_response_copy`,
  `matrix_draw_icon`, `volume_low_set`. **New deferred-extern found (NOT a bug):** `flash_read_config_with_crc_restore`
  (`0x08031784`, the config-*load* twin of `config_persist_dual_bank` — reads bank A, CRC-verifies, falls back
  to bank B and heals bank A on recovery) is a named-only extern; spec captured, a sourcing candidate.
  A further 10-function batch then ran (9 PASS) and **caught 1 more real bug — `modem_step_poweron`** drove the
  **wrong GPIO** for the modem main-supply enable: `MODEM_PWR_EN_PIN` was `0x0010` (PB4) but the OEM writes
  GPIOB **mask 0x4 = PB2** (confirmed by my own disasm: `r5`=GPIOB base, first `movs r1,#0x4`). Fixed the macro
  (also used by poweroff → both now correct) and the "PB4"→"PB2" doc references; also restored timer[0]'s OEM
  name `"interval_tmr"` (was `"timeout_tmr"`). The other 9 PASSed (`console_cmd_battery`/`_adc`, `modem_handle_uuloc`,
  `bms_telemetry_unpack`, `battery_charge_display_step`, `slip_send_frame`, `matrix_draw_level_bar`,
  `shifter_send_gear`, `led_driver_standby_write`).
  3 funcs sourced (decomp-c 515→518); modem prototype set in Ghidra + program saved.

- **Validation sweep, batches 2-5 (lean re-verification workflows) — 9 more real bugs found + fixed.**
  After the over-scoped 48-agent run bottlenecked on the single Ghidra MCP server, the remaining important
  functions were re-validated in lean ~11-agent batches. **Batch A (10 funcs + shifter.c hazard): all PASS,
  0 missing-arg bugs in shifter.c** (the bare-paren `shifter_send_gear()`/`shifter_crc_update()` calls are
  decompiler artifacts — the disassembly confirms r0 holds the value at the `bl`). **Batch B (11 funcs +
  states.c hazard): 6 PASS, 5 FIXED, 1 deferred-extern noted:**
  (1) **`sim_iccid_check` (modem.c) — address-vs-deref:** the ICCID is reached via a *pointer* stored at
  `ctx+0x3E8` (OEM `ldr [ctx,#0x3E8]` then `+0x50`), not a flat `ctx+0x3E8+0x50` offset — the anti-theft
  ICCID compare + log were reading the wrong address. Now `*(char **)(ctx+0x3E8)+0x50`.
  (2) **`modem_at_exec` (modem.c) — wrong arg:** the `>`-prompt echo passed `'\0'` instead of the `'>'` rx
  byte; `'\0'` fails the accumulator's printable filter so the `>` was dropped, breaking every `expect==">"`
  data-prompt wait (CMGS SMS send). Now passes `rx`.
  (3) **`HAL_I2C_Master_Transmit` (i2c.c) — missing `__HAL_I2C_CLEAR_ADDRFLAG`:** the SR1-then-SR2 read after
  `MasterRequestWrite` was omitted; without it SCL stays stretched and TX never advances. Added.
  (4) **`HAL_I2C_Mem_Read` (i2c.c) — same ADDR-clear missing in all 4 `XferSize` branches** (and the `>=3`
  else branch was absent entirely); reads would time out. Rewrote the selector to the canonical CubeF4
  ordering (==0 clear,STOP / ==1 ACK-off,clear,STOP / ==2 ACK-off,POS,clear / else clear).
  (5) **`led_driver_set_shipping_mode()` (states.c:1744) — missing arg:** the K&R extern hid a dropped
  argument (the OEM passes the post-decremented `G_STATE[0x27]`). Now `led_driver_set_shipping_mode(G_STATE[0x27])`.
  **Batch C (11 funcs + display.c hazard): 9 PASS, 2 FIXED, display.c hazard clean:**
  (6) **`HAL_I2C_Master_Receive` (i2c.c) — the SAME ADDR-clear omission in all 4 `XferSize` branches** as
  its Transmit/Mem_Read siblings (the third I2C HAL function with this bug) → receives would hang. Added the
  per-branch SR1/SR2 clear (==0 clear,STOP / ==1 ACK-off,clear,STOP / ==2 ACK-off,POS,clear / else ACK-on,clear).
  (7) **`ssp_rx_byte` (ssp.c) — extra dereference:** masked RXNEIE at `*(*(*(0x20009864))+0xC)` (three loads)
  where the OEM does two (`*(0x20009864)` is the UART5 base, `+0xC` is CR1) — so it poked a bogus address and
  never gated the UART5 RX IRQ around the ring read. Removed the extra hop (now matches the `uart_send_byte`
  idiom; verified the other byte-pumps don't share it). **The whole CubeF4 I2C transfer driver is now
  validated** (Transmit/Receive/Mem_Read fixed, Mem_Write/Init clean). PASSed clean across C:
  `HAL_I2C_Mem_Write`, `HAL_I2C_Init`, `usart2/3_irq_handler`, `uart5_irq_handler`, `sspm_bus_get_byte`,
  `mainware_boot_init_sequence`, `stc3115_i2c_read`, `light_pattern_action_apply`; display.c hazard sweep found
  0 missing-arg / address-vs-deref issues (the request setters correctly take descriptor addresses).
  **Batch D (10 funcs + ble.c hazard): 6 PASS, 2 FIXED, 2 deferred-externs, ble.c hazard clean:**
  (8) **`login_handler` (console.c) — AUTH-STATE DESYNC (address-vs-deref + struct-model):** the OEM (and our
  own `logout` + `console_edit`) write/read `logged_in`(+0x2D9)/`fail_count`(+0x2E0) at the **`g_app_state`
  base** (`0x20009368`), but `login_handler` wrote them through `ctx_sub` (→`0x200083A8`) — so a *correct*
  password set the wrong byte and the session never registered as logged-in (login effectively broken). Root
  cause: `logged_in`/`fail_count` were mis-declared inside `struct session_ctx` when they actually live in the
  outer `struct app_state` adjacent to `ctx_sub`. Fixed the struct model (moved both fields to `app_state` with
  new `_Static_assert`s at 0x2D9/0x2E0) and `login_handler` (now `g_app_state.logged_in/.fail_count`), matching
  the other two accessors. Also restored the OEM `"Please wait..\r\n"` lockout string (was missing `\r\n`).
  (9) **`flash_erase` (flash.c) — wrong log string:** logged `"Flash erase error %d"`; OEM string @`0x08052F18`
  is `"Sector error %d\r\n"`. Corrected (observable-output divergence). PASSed clean: `HAL_UART_Init`,
  `flash_write`, `config_persist_dual_bank`, `console_region_set`, `matrix_draw_number`, `modem_at_response_match`;
  ble.c hazard sweep found 0 missing-arg / address-vs-deref issues (config_persist by-value struct + request-
  setter descriptor addresses all correct).
  **Deferred-externs noted (named-only, no committed body — not bugs):** `flash_config_bank_write`
  (`0x080316D0`) and `save_state_record_to_eeprom` (`0x0803E2CC`) join `modem_sms_dispatch_command` as
  documented stack-aliasing-ABI named-externs; the verifiers captured each one's full OEM spec for a future
  reconstruction pass.
  **Deferred-extern noted (not a bug):** `modem_sms_dispatch_command` (`0x0803D668`, the inbound-SMS remote-
  command interpreter — the SMS half of the anti-theft surface) is a *named extern with no committed body*;
  the verifier captured its full OEM spec (8-char code, `#`/`*` delimiters, `key`/`TrackingOn`/`TrackingOff`/
  `ping`/`makeNoise` commands + their persist/BLE actions) for a future reconstruction. PASSed clean this
  pass: `battery_charge_lookup`, `ble_read_request_dispatch`, `pack_validate`, `boot_init_cold`,
  `light_sensor_read_step`, `lis3dh_accel_init`, the 10 of batch A. Build clean **text 1996**, 0 warnings.

- **Validation sweep of the already-sourced C (29-agent re-verification workflow) — 1 real bug fixed.**
  A curated high-risk set of **28 already-committed functions** (the UART4↔5/UART7↔8 swap-edited serial
  layer, the fixed-point/soft-float math, the SLIP/Modbus framing, and structural spot-checks of the six
  giant hand-assembled engines) was re-derived from raw OEM disassembly and diffed against the committed
  source; a 29th agent re-checked the whole serial-peripheral mapping chain (init Instance base → ISR home
  file → NVIC slot → `hardware.md`) for coherence. **Result: 25 PASS + 3 resolved.** (1) **REAL BUG FIXED —
  `bat_modbus_master_step` (battery.c case 2):** `bus_crc16_update()` was called with **no argument** (the
  K&R `extern int bus_crc16_update()` let it compile), so the Modbus function-code byte could be dropped
  from the running CRC and **every BMS response would fail the case-7 trailing-CRC check** — now
  `bus_crc16_update(rx)`, matching all 24 sibling calls; swept battery.c to confirm it was the only
  missing-arg call. (2)+(3) **behaviour-equivalent, documented (not changed):** `sspm_bus_recv_frame`'s
  `"PE\r\n"` log goes through `g_log_func` (printf slot) where the OEM uses the `puts` vtable slot — byte-
  identical output for a constant string, and consistent with how all of ssp.c models logging;
  `console_printf` kicks the watchdog unconditionally where the OEM throttles ~1-in-256 (IWDG never times
  out either way) and returns `len` vs the OEM's incidental-r0 (printf return is ignored) — both within the
  behaviour-equivalent goal, now noted in code comments. The serial-mapping coherence agent + the
  `uart4_irq_handler` verifier both PASS, independently re-confirming the UART4/5/7/8 correction. Build
  clean **text 1996**, 0 warnings.

- **Remaining bespoke glue SOURCED (12 funcs across new `comm.c` + `crc.c`/`rtc.c`/`modem.c`/`ble.c`/
  `sensor.c`/`startup_stm32f413.S`) — the bespoke application layer is now complete.** All build clean
  **text 1996**, 0 warnings, and **adversarially verified by a 14-agent workflow: 11/11 new decodes PASS
  (high confidence, no issues) + a 3-range completeness critic that examined 137 `FUN_` and found 0
  missed bespoke** (it could not falsify the completeness claim). **New `comm.c`/`comm.h`:**
  `comm_register_buffer` (`0x080317F4`, the 16-slot inter-module comm-buffer registry — pool @SRAM
  `0x2000069C`, one init-flag byte then 16×0xC records `{buf, size(u16), 3×u16 zeroed}`; first call
  zero-inits the occupancy field, alloc returns the first free slot) and `comm_buffers_register_all`
  (`0x08035D0C`, 16 registrations wiring each per-port context's RX/TX rings — bases `0x2000094C`/
  `0x20004DB4`/`0x20001A44`/`0x20002B3C`/`0x20003C34`; was an extern stub in `main.c`, now sourced).
  **`crc.c`:** `HAL_CRC_MspInit`/`HAL_CRC_MspDeInit` (`0x08040288`/`0x080402B8`, gate `RCC_AHB1ENR` bit 12
  CRCEN on/off, guarded on `hcrc->Instance == CRC 0x40023000`; MspInit keeps the CubeF4 read-back delay)
  and `crc_accumulate_device_uid` (`0x080402E8`, copies the 96-bit device UID `0x1FFF7A10` to a stack
  buffer → `HAL_CRC_Accumulate(handle 0x20009D90, buf, 3)`, returns the CRC via r0-passthrough; the
  `console_cmd_show` device hash). **`rtc.c`:** `rtc_msp_init` (`0x0803805C`, the `HAL_RTC_MspInit`
  override called from `HAL_RTC_Init` — sets `RCC_BDCR.RTCEN` via the peripheral bit-band alias
  `0x42470E3C` and enables NVIC IRQ 3 = RTC_WKUP, guarded on `hrtc->Instance == RTC 0x40002800`) and
  `rtc_wakeup_event_cb` (`0x08038A04`, the wake-up event latch fired by `rtc_wakeup_irq_handler`:
  `*(*(u32**)(0x20000094+0x24)) = 1`). **`modem.c`:** `sms_tracking_latch_once`/`sms_tracking_get`
  (`0x0803D648`/`0x0803D65C`, the SMS info-tracking one-shot latch + getter on flag @`0x2000839C`).
  **`ble.c`:** `ble_interval_debounce` (`0x0803A538`, used by `ble_telemetry_change_broadcast` — while
  any watched change bit is set it keeps an 8000-tick `scheduler_*` one-shot armed in `*slot` (timer
  name `"*tmr"` @`0x080529D0`) and reports `scheduler_slot_is_idle`; releases + reports ready when the
  bits clear; preserves the OEM 6-arg stack ABI). **`sensor.c`:** `adc_config_shadow_copy` (`0x08032CBC`,
  latches the live ADC sampling config `ADC_CTX+0x18/0x1c/0x20` into the shadow `+0x24/0x28/0x2c` while
  the status byte `+0x22` is clear; OEM loads `+0x20` as a word, stores its low half). **`startup.S`:**
  `0x08043EB8` was identified as the OEM **`Default_Handler`** (the spin target of the unused vector
  slots — already reconstructed in startup.S; renamed + marked no-return in Ghidra this pass, NOT a new
  function), and `reset_region_empty_stub` (`0x08043EC8`, an unreferenced no-op frame just past
  Default_Handler — exact instruction sequence reproduced for reset-region coverage). 12 Ghidra renames
  + 12 prototypes + 1 no-return, program saved. **pending 142→130, decomp-c 504→515; the JSON needs a
  `DumpMainwareProgram.java` re-dump.**

- **Console passthrough I/O-table installer → `console.c`** (1 func + 3 no-op stubs, PASS-verified,
  build clean **text 1996**) — `console_passthrough_io_install` (`0x08043148`, the survey's lone
  HIGH-value remaining item). On entering the `gsmdebug`/`bledebug` raw-UART passthrough,
  `light_tick_update` calls it to install a **silent** `g_log_func` table (`0x20009D98`): slots
  [0]/[2]/[3] printf/puts/write become the no-op stubs `0x08036B78`/`74`/`76` (all `bx lr`-class —
  created+named `io_noop_printf/puts/write`) so firmware log output can't corrupt the forwarded byte
  stream, while [1]/[4] tx/rx bind to the active console port (UART7, or USART1 when
  `g_console_port_sel` `0x20000114`==1). The two branches differ only in slots [1]/[4]; verified
  byte-for-byte against the literal pool @`0x08043180`. (Also added the prior `console_magic_sequence_match`
  prototype to console.h.) 1 rename + 3 functions created + 1 prototype, program saved.
  **pending 143→142, decomp-c 503→504.**

- **Triage clusters #3–#7 SOURCED (16 funcs across `ssp.c`/`battery.c`/`sensor.c`/`console.c`/`i2c.c`)**
  — the four HIGH + three MED clusters from the remaining-FUN_ triage, all build clean **text 1996**,
  0 warnings, and **adversarially verified by a 15-agent fan-out workflow (14 PASS + 1 confirmed FAIL,
  fixed → 16/16 faithful)**. **#3 SSPM RX → `ssp.c`:** `sspm_bus_recv_frame` (`0x0803A0C0`, the SLIP+
  CRC-16 RX de-framer — twin of `sspm_bus_send_frame`; SLIP state @`0x20007F94`, the returned status vs
  persistent state byte diverge per-branch, reproduced exactly), `tx_table_release_by_handle` /
  `tx_table_free_count` over the 16×0x18 endpoint table @`0x20007E14`, and the `sspm_ble_cmd_bridge` /
  `sspm_ble_read_bridge` tail-call bridges to the BLE dispatchers. **#4 announce → `battery.c`:**
  `manchester_announce_decode` (`0x08043B28`, decodes the staged inter-module announce frame types
  0 serial / 1 fw-version / 2 SoC / 3 SoH·NoC·WST into the telemetry cache + logs), `wst_status_to_string`
  (WST_NONE/DISCHARGE/CHARGE/BYPASS/UNKNOWN), `staged_msg_crc16` (Modbus CRC-16, implicit-r0 return), and
  `tim10_announce_period_cb` (`0x08043DE0`, TIM10 period-elapsed cb reloading CNT to Period−0x682 and
  committing the pending→active double-buffer @`0x200096D4`). **Verify-caught bug (fixed):** the `soc %d`
  log loaded the cached SoC with `ldrsb` (signed) — peer-controllable `msg[1]` ≥0x80 must print negative,
  so the arg is now `(int8_t)cache[0]`. **#5 HDC1080 → `sensor.c`:** `hdc1080_set_pointer` +
  `hdc1080_read` (I²C dev 0x80, 4-byte read → soft-float convert, temperature to 0.1 °C signed via
  `__aeabi_d2iz`, RH to % unsigned via `__aeabi_d2uiz`; constants 2⁻¹⁶/165/40/10/100, the −40 done as
  `__aeabi_dsub`). **#6 → `sensor.c`/`console.c`:** `sensor_deglitch_filter` (`0x08038ED4`, 6-sample ring,
  drop min+max, `(sum−max−min)>>2`) and `console_magic_sequence_match` (`0x08037188`, 5-byte rolling ring
  @`0x20005E20` matched against the literal `"\x1b[14~"` F4 sequence, a `light_tick_update` helper).
  **#7 I²C → `i2c.c`:** `i2c_tx_complete_callback` / `i2c_error_callback` (per-controller log; an I2C1
  error also calls `display_request_recovery`) and `i2c_bus_scan` (`diagnostics_run_step`'s 0..0xFE probe
  of both buses via `HAL_I2C_IsDeviceReady`). 16 renames + 13 prototypes, program saved.
  **pending 167→151, decomp-c 487→503; function_count stays 817.**

- **ES3 console command engine + VT100 key decoder → `console_edit.c`/`console_edit.h` (NEW)** —
  the interactive front-end that drives the 49-command console (the per-command handlers were already
  in `console.c`). 11 funcs, **10/10 adversarially verified**, build clean **text 1996**, 0 warnings.
  `console_cmd_match` (`0x08040904`, command/whitespace token match); the **9-slot input-history ring**
  `console_history_init`/`_rotate`/`_recall`/`_prev`/`_next` (`0804094C`/`080409A0`/`08041184`/`080411CA`/
  `080411FA`) over nine 0x51-byte line buffers at `0x20009368+i*0x51`, ring-ctrl block at ctx `+0x2E4`;
  **tab-autocomplete** `console_autocomplete_search`/`_apply` (`0804125C`/`08041232`, prefix-scan the cmd
  table via `bounded_strncmp`); and the **two command dispatchers** over the 49-entry table @flash
  `0x0804F5C4`: `console_dispatch_login` (`08040A00`) is the **pre-auth path** — every typed line before
  login is matched to the literal `"login"` command and run through its handler (the gate);
  `console_dispatch_command` (`080426BC`) is the logged-in path (match line → split args at first space →
  handler). The **VT100/ANSI escape decoder** `console_vt100_decode_key` (`080431A4`) reads keys via the
  `g_log_func` I/O vtable slot [4] and folds CSI (`ESC [`) / SS3 (`ESC O`) sequences into internal key
  codes 0x80–0x98 (arrows/F-keys/Home/End/Ins/Del/PgUp/PgDn), with a 10-tick scheduler timer to emit a
  lone `ESC`. Escape state at ctx `+0x368/+0x369`. **Follow-on (same module): the line editor**
  `console_line_editor` (`0x080434F8`) — the ~200-line glue loop pulling one decoded key and applying
  it (insert/overwrite, BS/DEL mid-line redraw via `\x1b[K`/`\x1b[1D`, Home/End/left/right, history
  up/down, tab-completion echo of the staged match, Enter→`console_dispatch_login`/`_command`, Esc→
  clear). Active line at session `+0x350`, in-line cursor `+0x36A`, logged-in flag `+0x2D9` (pre-login
  echoes `'*'`). Adversarial verify caught 3 real bugs (now fixed): Delete's reposition loop emits one
  fewer `\x1b[1D` than Backspace (`+1` start), Tab echoes `ac_match`/`ac_len` (not the line), and the
  pre-login unknown-command string is the British "recognised" while the logged-in one is "recognized".
  **Still out (not the editor):** `FUN_08043148`, a 3rd I/O-table installer pointing at undefined
  `0x08036B74/76/78`. 12 renames + 8 prototypes, program saved.
  **pending 179→167, decomp-c 475→487; function_count stays 817.**

- **EEPROM read surface → `eeprom.c`** (2 funcs, build clean **text 1996**) — the on-board
  AT24C EEPROM (device **0xA0**, I2C3) read side, joining the existing region writer.
  `eeprom_read_id_block` (`0x0803E138`) writes the **command byte 0xFA** then reads **6 bytes**,
  returning `(recv | xmit) & 0xFF` (0 = OK); probed once at boot. A user cross-reference from
  VanMoof firmware **1.9.x** identifies the identical routine there as **`Security_GetLockState`** —
  the 6 bytes are the bike's lock/security state (recorded in a Ghidra plate comment + the source
  comment; name derived from this binary's own disassembly per the separate-target discipline, the
  sibling name as confirmation). `eeprom_read_bounded` (`0x0803E174`, ex-`FUN_`) = the bounded
  `HAL_I2C_Mem_Read` primitive (rejects len 0 or a read past the 0x80-byte device) behind
  `eeprom_read_config_with_crc_fallback`. The no-bound-check sibling `eeprom_read_raw` (`0x0803E182`)
  was named (not sourced). 2 renames + 2 prototypes + 1 plate comment, program saved.
  **pending 183→181, decomp-c 472→474, named unchanged 135; function_count stays 817.**

- **STC3115 fuel-gauge driver → `stc3115.c`/`stc3115.h` (NEW)** — the ST STC3115 LiPo
  coulomb-counter/OCV gas gauge on **I2C3** (handle `0x20009B04`, shared with the LIS3DH),
  8-bit address **0xE0**. Sourced the transport + RAM-CRC + conversion + measurement layers
  (16 funcs, adversarially verified **15/15 PASS**, build clean **text 1996**): I²C primitives
  `stc3115_i2c_read`/`_write` (`0x080392C0`/`0x08039448`, reg-addr write then read/write N), the
  register accessors `stc3115_read_reg`/`write_reg`/`read_word`/`write_word`/`read_block`/`write_block`
  (`0x080393DC`/`0x080394A6`/`0x080393FE`/`0x080394BE`/`0x080392F4`/`0x0803948C` — read_reg/write_reg
  were extern stubs in `sensor.c`, now bodied), the 16-byte SRAM RAM-mirror layer `stc3115_ram_read`/
  `ram_write`/`ram_init`/`ram_crc8`/`ram_update_crc` (`0x08039302`/`0x0803949A`/`0x08039294`/`0x0803924C`/
  `0x08039280`; **g_stc3115_ram @0x20006E80**, signature **0x53A9** at [0], **CRC-8 poly 0x07** at [15]),
  the fixed-point `stc3115_conv` (`0x0803923C`, `((v*scale)>>11 +1)/2`), `stc3115_check_id`
  (`0x08039428`, reg 0x18==**0x14** part-ID gate), and the measurement burst-read
  `stc3115_read_measurements` (`0x0803930E`): reads regs 0..14 and converts SOC%/voltage(12-bit,
  scale 0x2333)/current(14-bit, scale 0x968)/temp(°C×10)/counter/OCV into the caller's struct
  (out[1..7]). The verifier confirmed the byte→register map and the current↔out[4]/voltage↔out[3]
  index split exactly. **Register map** documented in `stc3115.h` (0 MODE, 1 CTRL, 2-3 SOC, 4-5 COUNTER,
  6-7 CURRENT, 8-9 VOLTAGE, 10 TEMP, 13-14 OCV, 0x13 CC_CNF, 0x14 VM_CNF, 0x18 ID, 0x20.. RAM). The
  config/startup/SOC-algorithm layer (`stc3115_apply_config`, `startup_from_ocv`/`restore`,
  `init_device`, the periodic `stc_read`) was **named + prototyped** (extern, deferred — `apply_config`
  has decompiler-dropped varargs in its `write_word` calls that need disasm-level resolution; `stc_read`
  is the large SOC-tracking algorithm with 64-bit reciprocal-multiply divides). `sensor.c`'s existing
  externs are satisfied by the new module. 18 renames + 11 prototypes + 1 RAM label, program saved.
  **pending 201→183, decomp-c 456→472, named 133→135; function_count stays 817.**

- **Timer subsystem → `tim.c`/`tim.h` (NEW)** — the board's TIM bring-up, sourced
  (9 funcs, adversarially verified **7/7 PASS**, build clean **text 1996**). Board init
  wrappers (were named-extern, now bodied): `tim1_pwm_init` (`0x0803C4F4`) — **TIM1 three-channel
  PWM** for the lamp LEDs (Prescaler 0x960, Period 99; CH1/2/3 → HAL_TIM_PWM_ConfigChannel
  OCMode PWM1 0x60; ConfigBreakDeadTime), `tim6_init` (`0x0803C2E0`, psc 0x42/period 0x32),
  `tim7_init` (`0x0803C32C`, psc 0x4AF/period 10000), `tim10_init` (`0x0803C37C`, psc 0x5F/period
  5000, base only — no clock-source cfg), `tim_channel_enable_output` (`0x08027988` =
  HAL_TIM_PWM_Start core: CCxChannelCmd + advanced-timer MOE + CEN unless trigger-mode 6).
  **MSP callbacks reconstructed** (the GPIO-AF + RCC-clock + NVIC glue, were the last `FUN_` in
  the `0x0803C2xx–C5xx` block): `HAL_TIM_PWM_MspInit` (`0x0803C3AC`, TIM1: APB2ENR.TIM1EN +
  NVIC IRQ25), `HAL_TIM_Base_MspInit` (`0x0803C3EC`, TIM6→APB1.bit4/IRQ54, TIM7→APB1.bit5/IRQ55,
  TIM10→APB2.bit17/IRQ25 — matches the `vectors.c` IRQ map), `HAL_TIM_MspPostInit` (`0x0803C494`,
  TIM1 PWM pins **PE9/PE11/PE13 → AF1**), `HAL_TIM_PWM_MspDeInit` (`0x0803C5D0`). Cross-check: the
  four handles (HTIM1 `0x20009A84`, HTIM6 `0x20009A44`, HTIM7 `0x20009AC4`, HTIM10 `0x20009A04`)
  are byte-identical to the `HTIM*` handle addresses the vector trampolines pass to
  `HAL_TIM_IRQHandler`, and HTIM1 = the lamp PWM object base from `lighting.c`. The generic
  CubeF4 HAL TIM core (Base_Init/PWM_Init/SetConfig/ConfigClockSource/ConfigChannel/4×OCx_SetConfig/
  ConfigBreakDeadTime/CCxChannelCmd/PWM_DeInit) was **named + prototyped** (extern, vendor-class,
  bodies deferred), as were the non-TIM strays `HAL_MspInit` (global PWR+SYSCFG) and
  `i2c2_wait_bus_idle`. 18 Ghidra renames + 9 prototypes, program saved. **pending 219→201,
  decomp-c 447→456, named 124→133; function_count stays 817.**

- **NVIC interrupt vector layer → `vectors.c` (NEW) + UART4↔UART5 / UART7↔UART8 label
  correction** — decoded the 25 peripheral-IRQ trampolines (`0x0803CA20`–`0x0803CB64`) that
  occupy the external-IRQ slots of the table at `0x08020200`: thin wrappers the CPU enters on
  each NVIC line, forwarding to a HAL servicer, a serial byte-pump ISR, or an EXTI-line demux
  (sometimes after an application pre-hook). Named with CMSIS vector names
  (`RTC_WKUP_IRQHandler`, `EXTI0`/`1`/`2`/`3`/`4`/`9_5`/`15_10`, `DMA1_Stream1`/`DMA2_Stream0`,
  `TIM1_UP_TIM10`/`TIM6_DAC`/`TIM7`, `I2C1_EV`/`ER`, `I2C3_EV`/`ER`, `USART1`/`2`/`3`/`6`,
  `UART4`/`5`/`7`/`8`). Decoding the table required establishing **slot index == IRQ number**
  (verified by direct slot reads: USART1=37, UART4=52, USART6=71, UART7=82, UART8=83), which
  turned the vector table into an authoritative peripheral-identity oracle and exposed a
  **two-pair naming swap** in prior work: the functions labelled `uart4_*`/`uart5_*` and
  `uart7_*`/`uart8_*` were each paired to the *wrong* physical peripheral. Proof (3 independent,
  17/17 adversarially verified): (1) init Instance MMIO bases — `uart4_init` stored
  `0x40005000`=UART5, `uart5_init` stored `0x40004C00`=UART4; `uart7_init` stored `0x40007800`=
  UART7 was the body labelled `uart8`, etc.; (2) vector slots — IRQ82(UART7) → the console
  handler, IRQ83(UART8) → the BLE-debug handler; (3) the console port selector value is literally
  **7** (`g_console_port_sel`=7, reset magic `0x55AA5507`), i.e. UART**7**. **Corrected** across
  Ghidra (26 swap-renames via temp tokens + 25 trampoline + 9 leaf names, program saved) and the
  whole source/doc tree via an atomic digit-swap (4↔5, 7↔8 on `uart`/`UART` tokens) over
  `bus.c`/`console.c`/`main.c`/`ssp.c`/`uart.c` + their headers + `boot.md`/`hardware.md`/
  `progress.md`/`README.md` (excluding `console.md`, which already correctly said
  `bledebug→UART8`). Physical result: **UART7 = ES3 console primary** (`console.c`),
  **UART8 = BLE-debug link** (`uart.c`), **UART4 = BMS/battery Modbus 9600** (`bus.c`),
  **UART5 = BLE data link 115200** (`uart.c`). The 9 shared leaves the trampolines call
  (`rtc_wakeup_irq_handler`, `HAL_DMA`/`TIM`/`I2C_EV`/`I2C_ER_IRQHandler`, `exti4`/`exti9_5`/
  `tim6`/`tim7_app_hook`) are named extern (bodies decode-pending). The trampolines are NOT yet
  wired into `startup.S` (would pull undefined leaf symbols) — they compile and gc away like
  `main`'s super-loop until the handler closure is ready. Build clean **text 1996**, 0 warnings;
  47 Ghidra renames, program saved. **pending 253→219, decomp-c 422→447, named 115→124.**

- **USART1 console port (2nd ES3 console, twin of UART7) → `console.c`** — the **last**
  serial byte-pump peripheral, completing the USART byte-pump layer (every USART/UART now has
  a decoded ISR). USART1 carries the *same console* as UART7: handle-pp `g_usart1_dev_pp`
  `0x200098A4` (set by `usart1_init`), rings in the shared ctx `0x2000094C` at TX `+0x04` / RX
  `+0x08`. Six functions sourced (`usart1_tx_byte` `0x08035E28`, `usart1_puts` `0x08035E5C`,
  `usart1_write` `0x08035E6E`, `usart1_rx_byte` `0x08035E88`, the newly-**created**
  `usart1_printf` `0x08035EBC`, `usart1_irq_handler` `0x08035F98`) — byte-for-byte the UART7
  pattern (locked TX/RX primitives + the decompiler-elided RM0430 SR+DR error-clear block in the
  ISR, both confirmed in raw disassembly; all 6 adversarially verified PASS, implicit-r0 ABI
  preserved). **Dual-console finding:** UART7 and USART1 are two physical ports for the *same*
  ES3 console — they share the command-mode flag (`g_console_state+0x82`) and the log-echo flag
  (`g_console_log_echo` `0x20000083`) but track their own consecutive-ESC/-TAB counters (USART1
  at `+0x80`/`+0x81`, UART7 at `+0x83`/`+0x84`). The active port is whichever one's I/O table is
  bound into `g_log_func`: `usart1_io_table_install` `0x0804309C` selects USART1 (port selector
  `g_console_port_sel` `0x20000114` = 1), `console_io_table_install` `0x080430D8` selects UART7
  (= 7); the escape handlers swap between them. Per-port bootloader hand-off magic differs:
  USART1 writes `0x55AA5501`, UART7 writes `0x55AA5507` (both to SRAM `0x20000000`). `usart1_printf`
  is the USART1 `g_log_func[0]` twin of `console_printf` (same vsnprintf → optional epoch-prefixed
  log echo → synchronous drain-buffered TX). `usart1_io_table_install` named (extern, not sourced,
  like `console_io_table_install`). Build clean **text 1996**, 0 warnings; 6 renamed + 1 created +
  7 prototypes + 2 RAM labels; program saved. **function_count 816→817.**

- **USART2 / UART8 / UART7 byte-pump triads → `modem.c` / `uart.c` / `console.c`** —
  sourced the three *remaining* serial byte-transport peripherals, completing the
  USART byte-pump layer. Each is the same triad pattern as the prior ISR pass (locked
  TX/RX primitives that mask the CR1 interrupt-enable bit behind a DSB/ISB, plus a
  byte-pump ISR carrying the decompiler-elided RM0430 SR+DR error-clear sequence — all
  14 functions adversarially verified PASS, error-clear blocks confirmed bit-correct in
  raw disassembly, implicit-r0 return ABI preserved):
  - **USART2 = the GSM/SARA modem AT channel → `modem.c`.** `modem_uart_putc`
    (`0x080360F8`, single-byte TX), `modem_uart_flush` (`0x08036130`, the puts —
    already a caller-side extern, now sourced), `modem_uart_rx_byte` (`0x08036144`,
    already an extern, now sourced; returns 1 = got a byte — the old "0 = got" comment
    was wrong), `usart2_irq_handler` (`0x0803617C`). Handle pp `0x200099A4`
    (`g_gsm_dev_pp`), ctx `0x2000094C` (shared with USART3), TX ring `+0x410`, RX ring
    `+0x414`. Consumed by `modem_at_exec` (via the puts) and the command-mode bridge.
  - **UART8 = the BLE-coprocessor *debug* link → `uart.c`** (sibling of UART5's data
    link). `uart8_tx_byte` (`0x08036A38`), `uart8_rx_byte` (`0x08036A70`),
    `uart8_irq_handler` (`0x08036AA8`). Handle pp `0x200098E4` (`g_uart8_dev_pp`), ctx
    `0x20003C34`, TX ring `+0x970`, RX ring `+0x974`. Reached only via the `bledebug`
    console command's passthrough bridge.
  - **UART7 = the ES3 debug-console prompt → `console.c`.** `uart7_tx_byte`
    (`0x08036754`), `uart7_puts` (`0x0803678C`), `uart7_write` (`0x0803679E`),
    `uart_rx_ringbuf_get_byte` (`0x080367B8`, kept its name, now sourced),
    `uart7_irq_handler` (`0x080368D4`), and the previously-**undefined**
    **`console_printf`** (`0x080367F0`, newly created). Handle pp `0x200097E4`
    (`g_uart7_dev_pp`), ctx `0x20003C34` (shared with UART8), TX ring `+0x164`, RX ring
    `+0x168`. `console_io_table_install` (`0x080430D8`) installs these five as the
    `g_log_func` table (`0x20009D98`): `[0]=console_printf, [1]=uart7_tx_byte,
    [2]=uart7_puts, [3]=uart7_write, [4]=uart_rx_ringbuf_get_byte` — so **`console_printf`
    is `g_log_func[0]`, the firmware-wide printf** every module already calls.
    - `console_printf` formats with `vsnprintf` into a 256-byte stack buffer; when
      `g_console_log_echo` (`0x20000083`) is set it also mirrors an `"%ld "` epoch
      prefix + the message into the SRAM log. It then writes the message out the UART7
      TX ring **synchronously**: TXEIE masked while bytes are queued directly, and on a
      full ring it enables TXEIE and busy-waits (kicking the watchdog) until
      `ringbuf_free_space` reaches the cap (FIFO fully drained) before resuming; TXEIE
      left enabled on exit so the ISR sends the tail. (`ringbuf_free_space`
      `0x080318EC` = cap − count, newly sourced to `util.c`.)
    - `uart7_irq_handler` carries a **command-key escape handler**: each received byte
      is watched for ten consecutive ESC (`0x1B`) → write boot magic `0x55AA5507` to
      SRAM `0x20000000`, log `"NVICReset"`, `systick_delay(10)`, `system_reset()`
      (AIRCR, no return); or ten consecutive TAB (`0x09`) → `console_io_table_install()`,
      log `"To Commandmode"`, raise the command-mode flag (`g_console_state+0x82`,
      block `0x20004D2C`). The OEM re-reads DR three times (push + two compares),
      reproduced faithfully.
  - This also identified the mislabelled `light_tick_update` (`0x080371E8`) as the
    **command-mode console↔modem/BLE-debug bridge** (routes the console to USART2 via
    `gsmdebug` or UART8 via `bledebug`), not a lamp function (still named, out of scope).

- **USART RX/TX byte-pump ISRs → `shifter.c` / `uart.c` / new `bus.c` / `ssp.c`
  (+ `motor.c` finished)** — sourced the four USART interrupt service routines that
  move bytes between each serial peripheral's register block and its software ring
  buffers, each colocated with its peer byte primitives:
  - `usart3_irq_handler` (`0x080362D0`) → **`shifter.c`** — the eShifter (USART3)
    link; RX ring at ctx`+0xC20`, TX ring `+0xC1C`; handle pp `0x20009824`
    (`g_usart3_handle`), ctx `0x2000094C` (`g_usart3_rings`). Pairs with the existing
    `shifter_uart_tx_byte`/`rx_byte`.
  - `uart5_irq_handler` (`0x08036560`) → **`uart.c`** — the BLE-coprocessor (UART5)
    link; RX ring `+0xB40` (drained by `ssp_rx_byte`), TX ring `+0xB3C` (fed by
    `uart_send_byte`); handle pp `0x20009864`, ctx `0x20001A44`.
  - `usart6_irq_handler` (`0x0803669C`) → **`ssp.c`** — the inter-module SSPM bus
    (USART6); RX ring ctx`+0xA54`, TX ring `+0xA50`; handle pp `0x20009924`, ctx
    `0x20002B3C`. Pairs with `sspm_bus_send_byte`/`get_byte`.
  - `uart4_irq_handler` (`0x08036424`) + its two byte primitives **`bus_tx_enqueue_byte`**
    (`0x0803639C`) and **`bus_rx_byte_locked`** (`0x080363EC`) → **new `src/bus.c`** +
    `include/bus.h` — the UART4 BMS/battery bus (Modbus-RTU to slave `0xAA`, owned by
    `battery.c`); handle pp `0x20009964`, ctx `0x20001A44`, TX ring `+0x330`, RX ring
    `+0x334`. The two primitives were previously only scattered K&R `extern`s; both
    preserve the same TXEIE/RXNEIE mask + DSB/ISB + implicit-r0-return ABI quirk as
    `uart_send_byte`/`ssp_rx_byte`, so they now return `int` (the ring status).

  Each ISR: on **RXNE** (+ RXNEIE, and — for usart3/4/5 — no SR error flag set) push
  DR into the RX ring; on **TXE** (+ TXEIE) pop the next TX byte to DR, disabling
  TXEIE when the ring drains. **Crucially**, all four also run the RM0430 error-clear
  sequence — for each latched SR error bit (PE `0x1`/FE `0x2`/NE `0x4`/ORE `0x8`) read
  SR then DR to clear it (the CubeF4 `__HAL_UART_CLEAR_*FLAG` idiom, captured here as a
  small `usart_clear_error_flag` helper). Two OEM quirks preserved: **usart6** clears
  errors *first* (and omits the FE `0x2` clear), then gates RX on RXNE+RXNEIE alone,
  and re-loads the device handle fresh across the RX/TX halves; the other three sample
  SR/CR1 once and clear errors *between* the RX and TX phases. The initial Ghidra
  decompile elided the error-clear block as dead stack scratch — disassembly review
  caught it (reading DR has the observable hardware effect of clearing ORE), so it is
  reproduced for behaviour-equivalence. Also finished **`motor.c`**: decoded the body
  of `motor_post_update_cb` (`0x08030994`) — `g_log_func("Reset F2806\r\n")`, release
  the shared download timer slot (`0x20000075`), drop PB9 (motor reset). Build clean
  (`text 1996`, 0 warnings); all 7 functions adversarially verified instruction-by-
  instruction (PASS); 4 renamed + 6 prototypes, program saved.

- **Motor-DSP (F2806x) firmware-update FSM → `src/motor.c` (capstone — `motor.c` now
  complete)** — sourced `motor_fw_update_fsm_step` (`0x08030FF4`), the 14-state
  top-level engine that reprograms the motor controller via its C2000 SCI ROM
  bootloader, orchestrating the transfer pumps + transaction SMs of the two prior
  passes. Flow: enqueue an update-start inter-module message (state 0) → wait for the
  module ack (flag `0x400000`, state 1) → **GPIOB reset/boot-pin sequence** (PB9
  `0x200` = reset, PB10 `0x400` = boot-mode select) to drop the DSP into its serial
  bootloader (states 3–4, with an RX drain that the OEM calls with a **NULL sink**) →
  settle + **autobaud** (`'A'`/`'A'`, states 5–6) → upload a fixed 0x95E-byte handshake
  payload from flash `0x0804463C` echo-verified (state 7) → second autobaud (states
  8–9) → **validate the staged motor pack** at flash `0x080A0000` (magic `0xAA55AA55`,
  type byte `0xA1`, "Motorpcb Application: v%x.%02x.%02X (%s %s)" + " size %d bytes",
  state 0xA) → **stream the C28x boot-stream payload** (`pack+0x28`, length
  `*(pack+0xC)-0x28`, state 0xB) → success 0xC ("F2806-OK", bike-state `0x1D`, return 0)
  / failure 0xD ("F2806-err"/no log, bike-state `0x1B`, return 2). Progress logs
  F_init/F_reset/F_autobaud/F_upload/F_ready. Its control block is at `0x20000075`
  ([0] = the download timer slot the pumps share, [1] state, [2] aux slot, [3] flag);
  `download-ctx+0x3E` (`0x20000654`) records whether the bike entered the "updating"
  state and gates the exit transition. Returns 0 done / 1 busy / 2 failed / 3 waiting.
  - **String + arg fidelity:** all 14 format strings re-read byte-exact from the OEM bin
    (rodata, incl. the out-of-line `0x0804FCCC` "  ERROR SSP place" and `0x0804FA1C`
    " size %d bytes"); the case-0xA 6-arg version log resolved from the **disassembly**
    (r1=`ver>>24`, r2=`(ver>>16)&0xFF`, r3=`(ver>>8)&0xFF`, two stacked `%s` pointers
    `pack+0x10`/`pack+0x1C`) since the decompiler dropped them.
  - **Two-region body:** the function's code is split by a literal-pool island (main
    `0x08030FF4`–`0x08031296`, continuation `0x080312E8`–`0x080313B0`); both pools fully
    resolved. **Created + named** the previously-undefined 30-byte one-shot scheduler
    callback `motor_post_update_cb` (`0x08030994`, armed 200 ticks after the reset for
    both exits — body decode-pending).
  - **Adversarially verified** instruction-by-instruction (all 15 states PASS): the
    `tbh` jump table, case-2-returns-3 vs default-returns-1, the case-1 double
    `scheduler_release`, the magic/type checks, every GPIO pin/level, scheduler timeout,
    state-byte write and return value confirmed. Build clean (`text 1996`, 0 warnings);
    prototype set + 1 new function created, program saved. **`motor.c` (8 funcs) now
    covers the entire mainware→motor-DSP update path**; only `motor_post_update_cb`'s
    body remains (a small leaf).

- **Motor-DSP (F2806x) transaction state machines → `src/motor.c`** — the handshake/
  transfer layer that `motor_fw_update_fsm_step` drives on top of the four transport
  pumps (prior pass). All three share the same download context (`0x20000654`) at
  higher offsets and log via `g_log_func` (`0x20009D98`):
  - `motor_dl_send_verify_step` (`0x08030BFC`) — send `count` bytes, verifying each is
    **echoed** back (the C2000 SCI bootloader echoes every byte): per-byte send (state 1)
    then receive+compare (state 2). Echo mismatch (non-final) logs "Fail 2 %d" + aborts;
    final-byte mismatch silently completes (OEM quirk). ctx slice: +0x20 state, +0x24
    cursor, +0x28 remaining.
  - `motor_dl_autobaud_step` (`0x08030CEC`) — the SCI **autobaud lock**: send `'A'`
    (0x41), expect `'A'` echoed → "Autobaud ok" / "Err Autobaud [%d]" / "Autobaud no
    answer". ctx +0x2C state.
  - `motor_dl_stream_block_step` (`0x08030D88`) — 6-state length-prefixed block streamer
    with a running 16-bit additive checksum verified against the DSP's echoed checksum
    (per-block in state 4, periodic every 2048 bytes in state 5). Block length parsed
    from the first two streamed bytes (which are themselves also summed into the
    checksum). End-of-block = counter == `(length+3)*2`. ctx +0x2D state, +0x30 cursor,
    +0x34 checksum, +0x38 counter, +0x3C block length.
  - **String fidelity:** all 14 log format strings re-read byte-exact from the OEM bin
    (rodata 0x08050D20–0x08050DEC), and — per the standing discipline — **every
    `g_log_func` argument resolved from the disassembly, not the decompiler** (which
    drops the variadic register args): "Fail 2 %d"/"Err Autobaud [%d]" carry the
    r1-leftover value (count / received byte), and the two `%04X %04X` strings pass
    (received_u16, computed_checksum) in r1/r2.
  - **Adversarially verified** instruction-by-instruction (all 3 PASS): the case-3 `tbh`
    jump table, the no-double-advance length/checksum reads, the `len==0 && cnt>1`
    returns-0 path, the (cnt-6)&0x7FF periodic test, and the per-case return values
    (mismatch returns the old state value 2; case4 clears +0x38 on match but case5 does
    not) all confirmed. Build clean (`text 1996`, 0 warnings); 3 renamed + 3 prototypes,
    program saved. **Next pass:** the `motor_fw_update_fsm_step` body (`0x08030FF4`, ~14
    cases — GPIO reset pulses, pack-header parse, state orchestration; "Resetting motor"
    @0x08050DF0 onward) completes `motor.c`.

- **Motor-DSP (F2806x) firmware-update transport → NEW `src/motor.c` + `include/motor.h`
  (+ RX byte primitive → `src/ssp.c`)** — sourced the transport foundation of the
  mainware→motor-controller firmware updater. The motor MCU is a TI **F2806x**
  (TMS320F28054F, C28x DSP); the OEM translation unit is literally
  **`src/F2806/f2806x.c`** (its assert-filename string @rodata 0x08050D0C), and the
  whole block (`0x080309C4`–`0x08030FF4`) reprograms the DSP over the inter-module
  ("SSPM") bus with a YMODEM-like block protocol under `motor_fw_update_fsm_step`
  (`0x08030FF4`, already named). This pass took the bus RX byte primitive + the four
  non-blocking transfer pumps; the three transaction SMs (`0x08030BFC`/`0x08030CEC`/
  `0x08030D88`) and the top FSM body are the **explicit next pass** (string-heavy).
  - `sspm_bus_get_byte` (`0x08036664`, was `FUN_`) → **`ssp.c`**: the exact RX twin
    of `sspm_bus_send_byte` — same device-handle pointer (0x20009924), but masks/sets
    **RXNEIE (CR1 bit 5, 0x20)** and reads the **RX** ring at bus-ctx slot **+0xA54**
    (0x20002B3C) via `ringbuf_get_byte`; same ABI quirk (implicitly returns the get
    status, 0 = ring empty). Also called by the SSPM RX de-framer `FUN_0803A0C0`
    (counterpart of `sspm_bus_send_frame`, future).
  - The four pumps → **`motor.c`**: `motor_dl_send_buf_step` (`0x08030B70`, sends N
    bytes from a cursor via `sspm_bus_send_byte`), `motor_dl_recv_buf_step`
    (`0x080309C4`, receives N bytes via `sspm_bus_get_byte`), `motor_dl_recv_byte_step`
    (`0x08030A50`, one byte), `motor_dl_recv_u16_step` (`0x08030ADC`, little-endian
    u16). All four are one-byte-per-tick state machines that share **one** transfer
    context (`0x20000654`, disjoint slices: recv-buf +0x00/+0x04/+0x08, recv-byte
    +0x0C, recv-u16 +0x0D/+0x0E/+0x10, send-buf +0x14/+0x18/+0x1C) and **one**
    scheduler-timeout slot byte (`0x20000075`); they assert (not allocate) if the slot
    is still `SCHED_SLOT_NONE` (0xFA) — the FSM allocates it first. Return convention
    **1 = busy / 0 = complete / 2 = timed out**. Exact OEM assert line numbers
    preserved (0x161/0x197/0x1C9/0x1F5). Note: `recv_u16` **sets** state=1 at arm
    whereas the other three **increment** from 0 — reproduced exactly.
  - **Adversarially verified** instruction-by-instruction (all 5 PASS): ctx offsets,
    assert line numbers, increment-vs-set, RXNEIE/TXEIE bit, deref levels, and the
    0/1/2 return mapping all match. Build clean (`text 1996`, 0 warnings); 5 renamed +
    5 prototypes, program saved.

- **SSPM inter-module-bus TX path → `src/ssp.c` + `include/ssp.h`** — sourced the
  two outbound primitives for the *second* serial link (mainware↔battery/motor/
  shifter, distinct from the BLE-coprocessor UART that `uart_send_byte` drives):
  `sspm_bus_send_byte` (`0x0803662C`, was `FUN_0803662C`) and `sspm_bus_send_frame`
  (`0x0803A008`, previously only a named `extern` in `ssp.c` — and wrongly typed
  `void`: the disassembly returns `uint`, 0 / 2). `sspm_bus_send_byte` is the
  structural twin of `uart_send_byte`: device reg block via the pointer at
  `0x20009924`, TX-ring handle at fixed slot `+0xA50` into the bus context at
  `0x20002B3C`; it masks TXEIE (CR1 bit 7, with a DSB/ISB pair) around the ring
  push and re-enables it after re-loading the handle — same ABI quirk (implicitly
  returns `ringbuf_push_byte`'s r0 status; 0 = ring full). It has two callers
  (`sspm_bus_send_frame` and `FUN_08030b70`, a scheduler-paced buffer drainer left
  for later), so it is exposed (not static) in `ssp.h`. `sspm_bus_send_frame` is
  the SSPM-bus counterpart of `slip_send_frame`: SLIP framing (`0xC0` delimiters,
  `0xC0→DB DC` / `0xDB→DB DD`) with a little-endian CRC-16 (poly `0xA001`, init
  `0xFFFF`) trailer; returns 2 only if the closing `0xC0` can't be queued (matching
  the OEM `cbz` at `0x0803A0A6`). Both adversarially verified instruction-by-
  instruction against the disassembly — faithful, no discrepancies. Build clean
  (`text 1996`, 0 warnings); renamed 1 + 2 prototypes, program saved.

- **GPIO HAL deinit + EXTI ISR (CubeF4 `stm32f4xx_hal_gpio.c`)** — completed the
  GPIO HAL surface in **`src/gpio.c`** + **`include/gpio.h`**: `HAL_GPIO_DeInit`
  (`0x08026990`) and `HAL_GPIO_EXTI_IRQHandler` (`0x08026AD4`); Init/ReadPin/WritePin
  were already done (vendor-stock). Confirmed stock CubeF4 from the literal pools
  (SYSCFG `0x40013800`, GPIOA `0x40020000`, EXTI `0x40013C00`). `HAL_GPIO_DeInit`
  walks pins 0..15: for each selected pin it checks `SYSCFG->EXTICR[pos>>2]` still
  maps the line to this bank (open-coded `GPIO_GET_INDEX`, A=0..H=7) before clearing
  `EXTI->{IMR,EMR,RTSR,FTSR}` + the EXTICR nibble, then unconditionally returns the
  pad to reset (MODER/AFR/PUPDR/OTYPER/OSPEEDR). The OEM register-write **order is
  not the textbook stock-source order** — it is MODER(`+0x00`)→AFR(`+0x20`)→
  PUPDR(`+0x0C`)→OTYPER(`+0x04`)→OSPEEDR(`+0x08`); the C reproduces that exact
  order and offset mapping. `HAL_GPIO_EXTI_IRQHandler` is the 4-line pending-bit
  demux (`if (EXTI->PR & pin) { EXTI->PR = pin; HAL_GPIO_EXTI_Callback(pin); }`).
  Added the `GPIO_TypeDef`/`EXTI_TypeDef`/`SYSCFG_TypeDef` register blocks to
  `gpio.h`. The application's overriding `HAL_GPIO_EXTI_Callback` (`0x08038964`, a
  real ~150-byte body, called directly by the ISR) was **named** + prototyped this
  pass but its body left for a future pass (still `FUN_`-class work, not stock).
  Adversarially verified against the raw Thumb-2 disassembly — all three faithful:
  the non-textbook write order, every mask width (2-bit MODER/PUPDR/OSPEEDR, 1-bit
  OTYPER, 4-bit AFR/EXTICR), the `position>>2`/`(position&3)<<2` EXTICR index math,
  the A..H index table, and the rc_w1 PR-clear semantics all confirmed. Build clean
  (`text 1996`, 0 warnings; gc'd like the rest — nothing roots it yet).

- **UART HAL init/config/deinit (CubeF4 `stm32f4xx_hal_uart.c`)** — sourced the
  UART bring-up path into **`src/uart.c`** + **`include/uart.h`** (`docs/uart.md`):
  `HAL_UART_Init` (`0x08026CFC`), the static `UART_SetConfig` (`0x08026AF0`), and
  `HAL_UART_DeInit` (`0x08026D5A`). This is the foundation beneath all eight
  `uartX_init`/`usartX_init` wrappers (modem, e-shifter, display, inter-module
  buses). Confirmed stock CubeF4 from the literal pools: handle layout matches
  `UART_HandleTypeDef` exactly (`Instance@+0x00`, `Init@+0x04`, `Lock@+0x38`,
  `gState@+0x39`, `RxState@+0x3A`, `ErrorCode@+0x3C`); `UART_SetConfig` reproduces
  the CR1/CR2/CR3 framing masks and the `UART_DIV_SAMPLING8/16` BRR math
  (OVER8 → `(pclk*25)/(2*baud)`, frac `(rem*8+50)/100`, BRR
  `mant<<4 | (frac&0xF8)<<1 | frac&0x7`; OVER16 → `/(4*baud)`, frac
  `(rem*16+50)/100`, BRR `mant<<4 | frac&0xF0 | frac&0xF`), with **PCLK2** for the
  APB2 instances (`USART1`/`USART6`/`UART9`/`UART10` at `0x40011000 + n*0x400`)
  and **PCLK1** for the rest. The OEM `pclk*25` numerator is a 64-bit dividend
  (`__aeabi_uldivmod`, `0x08020AE4`); modelled behaviour-equivalently as a
  `uint64_t` divide (the `0x51EB851F`/`>>37` ÷100 magic written as `/100`). The
  runtime byte path stays the IRQ + ring-buffer `uart_send_byte` (already in
  `uart.c`); the blocking `HAL_UART_Transmit`/`Receive` are not used by this
  firmware. Adversarially verified against the raw Thumb-2 disassembly — all
  three faithful, the OVER8/OVER16 branches confirmed not swapped, every bitmask
  exact. Externs kept out of scope: `HAL_UART_MspInit` (`0x080333C0`,
  per-USART GPIO-AF + RCC-enable + NVIC), `HAL_UART_MspDeInit` (`0x08033740`),
  `HAL_RCC_GetPCLK1Freq` (`0x08027374`), `HAL_RCC_GetPCLK2Freq` (`0x08027394`).
  Build clean (`text 1996`, 0 warnings). Triage note: the adjacent GPIO HAL is
  one pass from complete — `HAL_GPIO_DeInit` (`0x08026990`) and
  `HAL_GPIO_EXTI_IRQHandler` (`0x08026AD4`) remain `FUN_` (Init/ReadPin/WritePin
  already done).

- **I²C HAL driver (CubeF4 `stm32f4xx_hal_i2c.c` transfer layer)** — sourced the
  entire blocking master/memory I²C driver into **`src/i2c.c`** + **`include/i2c.h`**
  (`docs/i2c.md`): 16 functions, the foundation beneath *every* on-board I²C
  peripheral (config EEPROM, STC3115 gas-gauge, LIS3DH, audio amp `MAX9768`, the
  two IS31FL3236 display drivers, the LED driver, light sensor). The central
  `HAL_I2C_Master_Transmit` (`0x08024760`) alone has 26 xrefs across those drivers.
  Layers reconstructed from `0x08023EEC..0x08025170`: the flag-wait helpers
  (`I2C_WaitOnFlagUntilTimeout`/`…MasterAddress…`/`…TXE…`/`…BTF…`/`…RXNE…` +
  `I2C_IsAcknowledgeFailed`), the address/memory request helpers
  (`I2C_MasterRequestWrite`/`Read`, `I2C_RequestMemoryWrite`/`Read`), the blocking
  transfers (`Master_Transmit`/`Receive`, `Mem_Write`/`Mem_Read`), and `HAL_I2C_Init`
  /`DeInit`. The handle is the stock `I2C_HandleTypeDef` (pBuffPtr@`+0x24`,
  XferSize@`+0x28`, XferCount@`+0x2A`, XferOptions@`+0x2C`, PreviousState@`+0x30`,
  Lock@`+0x3C`, State@`+0x3D`, Mode@`+0x3E`, ErrorCode@`+0x40`); the flag tokens
  decode to the exact CubeF4 `I2C_FLAG_*` `(reg<<16)|bit` values
  (`BUSY=0x00100002`, `ADDR=0x00010002`, `ADD10=0x00010008`, `BTF=0x00010004`,
  `XferOptions=I2C_NO_OPTION_FRAME=0xFFFF0000`). `HAL_I2C_Init`'s clock math is
  the standard `I2C_FREQRANGE`/`I2C_RISE_TIME`/`I2C_SPEED` (the `0x431BDE83` ÷1e6
  and `0x10624DD3` ÷1000 magic-multiplies modelled as the plain divisions they
  implement; PCLK1 ≥2 MHz std / ≥4 MHz fast guards; the `(field+1)&0xFFF==0→1` and
  `(ccr&0xFFC)==0→4` clamps). `i2c3_handle_init`/`_deinit` were refactored onto the
  struct and `HAL_I2C_Init`/`DeInit` are now real bodies (the externs are gone).
  The MSP/clock helpers `HAL_I2C_MspInit`/`MspDeInit` (GPIO+NVIC+RCC, `0x0803C69C`/
  `0x0803C820`) and `HAL_RCC_GetPCLK1Freq` (`0x08027374`) stay externs — a separate
  MSP/RCC cluster. **Adversarial verify** (one read-only pass over all 16 vs the
  disassembly) confirmed 15 faithful and caught one access-fidelity slip: the
  ADDR-clear in `I2C_RequestMemory{Write,Read}` must read **SR1 then SR2** (the C
  had read only SR2 with a wrong comment) — fixed to emit both volatile reads.
  Confirmed subtleties: `Master_Receive`'s pre-loop N≥3 setup sets ACK in an
  `else` branch, but `Mem_Read`'s deliberately does **not** (ACK already set in
  `I2C_RequestMemoryRead`); the RXNE wait's timeout is unconditional (no
  `HAL_MAX_DELAY` escape) and its STOPF path leaves ErrorCode untouched;
  `I2C_ERROR_WRONG_START` (`0x200`) is written on a START-still-set SB timeout.
  Build clean (`text 1996`, 0 warnings; gc'd like the rest — nothing roots it yet).

- **LIS3DH accelerometer driver** — sourced the whole motion-sensor driver into a
  new **`src/lis3dh.c`** (`docs/lis3dh.md`): 26 functions via a resolve → translate
  → adversarial-verify workflow (9 agents, both translate + verify per group). The
  ST **LIS3DH** hangs off **I2C3** at 8-bit addr `0x33` (7-bit `0x19`); a per-device
  vtable `g_lis3dh_dev` @ SRAM `0x2000838C` (`{write 0x0803D098, read 0x0803D074,
  wait 0x0803D070}` + WHO_AM_I scratch) abstracts the blocking HAL transport
  (`HAL_I2C_Mem_Read/Write(.., 0x33, reg|0x80, 1, .., 50ms)`). Layers: 3 transport
  leaves, 3 vtable thunks (`0x0802E434/444/44E`, confirmed to propagate the callee
  status), 15 register helpers (`0x0802E45E..0x0802E782` — CTRL_REG1-5 / REFERENCE /
  WHO_AM_I / INT1_CFG/SRC/THS/DURATION, all read-modify-write with the `wait→return 3`
  shape), and the public API `lis3dh_accel_init`/`config_motion_int`/`powerdown`/
  `int1_clear` (+ private `lis3dh_int1_read_source`). `config_motion_int` is the
  textbook motion-wake setup (HPF on INT1, `I1_IA1`, ±2 g, 100 Hz, OR-of-X/Y/Z high
  events); `status_process` arms it `(0,6)` high / `(0,0x20)` low. Verify caught one
  real refinement — the `else`-branch INT1_CFG mask is `& 0xD7` in the asm (not the
  decompiler-folded `& 0x57`); the asm-faithful `(cfg & 0xD7) | 0x02` then unconditional
  `& 0x7F` is used. **Correction:** `lis3dh_accel_init` calls `NVIC_DisableIRQ`
  (`0x080270FC`, `NVIC->ICER`), i.e. it **masks** EXTI IRQ `0x48`/`0x49` during the
  probe — `hardware.md` previously said "enables", now fixed. Also resolved the
  adjacent **`charge_time_estimate_reset`** (`0x0802E40C`, was grouped by flash
  proximity but NOT accelerometer): re-arms the 10 s BMS poll (`G_STATE[0x1d]`) and
  forces the charge-time estimate at app-ctx+`0x3FE` to the `0x8300` (-32000) invalid
  sentinel — **sourced → `states.c`**. Build clean (`text 1996`, 0 warnings).

- **Modem AT response/command callback layer** — sourced the last un-decoded
  layer of `modem.c`: the 32 `.build_cb` / `.handle_cb` targets of the flash
  AT-script table (`0x0802F1DC..0x0802FD68`) → `modem.c` (resolve → reconstruct →
  adversarial-verify workflow, 14 agents; build clean, `text 1996`). The layer is
  four parsing primitives (`modem_skip_to_cr`/`modem_skip_to_space`/
  `modem_extract_field`/`modem_at_response_copy`), the per-command response
  handlers (CGMI/CGMM/CGMR/CGSN/CIMI identity → the `0x20009CC0..0x20009D00`
  scratch block; CCID→`g_modem_iccid`; CPIN/CSQ/CREG/CMGF/UPSND/UUHTTPCR/CPMS/CMGL
  status; UGSRV/UGAOP/UPSD operator/AssistNow/APN checks; UULOC location-report
  builder), and the snprintf command builders (CPIN=/CMGS/CMGR/CMGD/SMS-body/
  UPSD/UHTTPC/UHTTP/UGAOP). Helpers resolved: `FUN_08021BDC`=`strstr`,
  `FUN_0802133C`=`strchr`, `FUN_0802181C`=`bounded_strncmp` (0 == equal). The CMGR
  handler feeds **`modem_sms_dispatch_command`** (`0x0803D668`) — the inbound-SMS
  remote-command interpreter (`#<code>*<cmd>` → unlock / factory-reset / state /
  location / bell), the SMS half of the anti-theft surface (named, body not yet
  sourced). **Adversarial verify caught 2 cursor-threading bugs** (`CMGR`/`UPSD`
  passed the response start to the skip primitives instead of threading the
  `strstr`/`extract_field` cursor — fixed). Full callback + scratch-global map in
  `docs/modem.md`. Triage note: the remaining `FUN_` are mostly newlib/libgcc
  (the `0x08020xxx` region) and HAL statics; the next coherent app cluster is the
  `0x0802E40C..0x0802E782` I²C register-device driver (`{write,read,poll}` vtable).

- **Sleep + reset/factory-reset state machines** — sourced the two long-deferred
  `status_process`/loop control functions into `states.c` (resolve → reconstruct →
  adversarial-verify workflow, 10 agents, 156 symbols resolved; build clean, `text
  1996`). A boundary surprise drove the framing: decompiling `reboot_restart_task`
  (`0x08038A68`) returned a glued ~250-line state machine with a bogus
  `unaff_r4 * 0x1000000` base. The cause was a **false fall-through** — Ghidra
  hadn't marked `FUN_08038A14` no-return, so it ran the literal pool at the end of
  `reboot_restart_task` straight into the *next* function. `FUN_08038A14` is
  **`NVIC_SystemReset`** (DSB; `SCB->AIRCR = 0x05FA0004 | (AIRCR & 0x700)`; DSB;
  spin) — marked no-return, after which three distinct functions resolved cleanly:
  - **`enter_stop_mode`** (`0x080382D0`, `void(uint8_t reason)`) — low-power STOP
    entry / wake-arm sequencer. Logs `"EnterSTOPMode %d min "` + a per-reason wake
    string (`Wake:All`/`NoMems`/`RST`/`Shipping`/`No bat`/`ERROR`), runs the
    peripheral de-init cascade (8 UART veneers, I2C×2, SPI, ADC, an AHB1 periph),
    parks all of GPIOA..H to analog (enable→Init→disable the AHB1ENR clocks), arms
    a per-reason EXTI wake-pin map on GPIOC/GPIOD (`GPIO_MODE_IT_FALLING/RISING`) +
    the RTC wakeup timer (skipped for reason 6 "Shipping"), then `enter_low_power_wait`
    (PWR STOP + **WFI** — sleeps here) and on wake re-inits the clock tree
    (warm/cold from the `0x20000000` marker) and `system_software_reset()`s. The
    per-reason sleep duration + wake mask live in one runtime RAM struct
    `@0x20000094` (slot[reason] u32 ms; its low u16 = RTC wake seconds; `+0x24` =
    write-through clear-ptr). Two **critical** reconstruction bugs were caught by
    the adversarial pass and fixed: the wake-mask read used stride 2 instead of the
    OEM's stride-4 `ldrh [base,reason<<2]`, and a stray pad pushed the clear-ptr off
    its `+0x24` offset.
  - **`reboot_restart_task`** (`0x08038A68`, `void(void)`) — the *actual* function:
    a 5-line reboot wrapper (clear warm marker → cold boot, log `"NVICReset"`,
    20 ms delay, `NVIC_SystemReset()`). Armed as a scheduler callback by
    `console_cmd_reboot`, the OAD-failed path, and `factory_reset_sm_step`.
  - **`factory_reset_sm_step`** (`0x08038A90`, `void(uint8_t *ctx)`) — the 6-state
    USER/factory-reset & power-cycle machine ticked from `main()` (`g_reset_sm`
    `@0x20006E44`: `[0]`=state, `[1]`=sub-step). Drives "USER Reset" → BLE notify
    0x11D → `gas_gauge_reset`/`settings_factory_reset`/15-word EEPROM save → NVIC
    reset, with a state-5 hardware-reset cascade (BLE/eShifter/`"\nReset BMS\r"`
    over the inter-module bus/Motor). Verdict: faithful.

  `~27` `FUN_` callees named in Ghidra this pass (prototypes + plate comments,
  program saved): `system_software_reset` (a second inlined `NVIC_SystemReset` @
  `0x080382AC`, no-return), `uart_handle_deinit_0..7` + the shared `uart_handle_deinit`
  /`HAL_UART_MspDeInit` workers, `i2c_handle_deinit`/`spi_handle_deinit`/
  `adc_handle_deinit`/`ahb1_periph_handle_deinit`, CMSIS `nvic_set_priority`/
  `nvic_enable_irq`/`nvic_clear_pending_irq`, `enter_low_power_wait`, `systick_irq_
  disable`/`enable`, `set_wakeup_done_flag`, `rtc_set_wakeup_seconds`/`rtc_wakeup_
  timer_disable`/`rtc_wakeup_timer_set`, and `reset_ble_timeout_cb` (`0x08038A38`,
  newly carved). The logger object `@0x20009D98` is confirmed a 3-slot fn-ptr table
  (`[0]`=printf=`g_log_func`, `[1]`=putchar, `[2]`=alt-formatter). Note kept for a
  future pass: `shifter_usart3_reinit` (`0x080338B4`, already sourced in `shifter.c`
  as a "re-init/flush") is actually the 3rd UART **de-init** veneer — a misnomer to
  revisit, left as-is to avoid destabilising existing verified code.

- **Backup-code investigation + name correction** — chasing "what is the default
  backup code?" exposed a misnomer: the leaf I'd called `backup_code_init_default`
  (OEM `0x0803FAC0`) does **not** touch the backup code. It seeds the three
  **sound-group volume-tier masks** at `ctx+0xF4/F8/FC` (`console_cmd_show`'s
  "Group low/medium/high %08X") to `{0, 0x383F33FE, 0x47C0CC00}` — medium∣high
  partition sound bits 1..30 disjointly. Renamed to **`sound_groups_init_default`**
  (Ghidra + `app.c`/`main.c`, prototype + plate comment set, literals labelled
  `g_dwSoundGroup{Medium,High}Default`, program saved); the old extern in `app.c`
  also carried a wrong OEM address (`0x0803FC50` is interior to
  `region_speed_preset_table_load`). **The real backup code** is the `uint16` at
  `ctx+0x100`: a 3-digit owner code (`value = d0 + d1·10 + d2·100`, 0..999)
  programmed over BLE `CMD_BLE_SECURITY_BACKUP_CODE` (handler `0x080349ac`, packs
  payload[0..2]); entered at the bike via horn presses (`states.c` "No backupcode").
  Factory reset writes `0x00FF` = "not set" sentinel — **there is no factory-default
  backup code; the bike ships with it unset** (`console_cmd_show` → "backupcode not
  set"). Documented in `docs/hardware.md` (`g_ctx` field map). Build clean, `text 1996`.
  Follow-up: decoded the rest of the config block from the `show` format strings and
  rewrote `settings_factory_reset` (app.c) from flat magic-offset writes to a
  documented `bike_config_t` overlay — sound groups, backup code, dark threshold
  (lux), sound volumes low/med/high `+0x105/6/7`, and the per-region pedal-assist
  "moment" presets (`+0x10E` engage / `+0x126` release, `[4][3]` u16 in 0.1 km/h)
  now read by name, with `_Static_assert`s pinning every field to its OEM offset.
  Behaviour-identical (final memory state + persist call unchanged), `text 1996`.

- **Named-leaf sourcing pass** — the 25 leaf functions the BLE/app/state modules
  still called as opaque `extern`s (named in a prior pass, bodies undecided) were
  decoded + adversarially verified (a decode→refute workflow, 50 agents, each
  cross-checking the live disassembly + OEM bytes), then **24 sourced into their
  home modules** (build clean throughout, `text 1996`):
  - **ble_read telemetry surface** — `ble_get_charge_plug_state` + the 0x60-byte
    `ble_build_testmode_versions_blob` → `ble_read.c` (its read-surface block now
    `#include`s the home headers and drops the opaque-extern thicket);
    `charge_level_adc_get` + `hw_version_lookup` (16-entry HW-ID divider table @
    flash `0x08044F9C`, ADC ctx `0x20000914`) → `sensor.c`; `gpio_pc0_is_low` /
    `gpio_pc1_is_low` (PC0/PC1) → `gpio.c`; `ble_get_led_channel_state` (g_lights
    `0x20006DC0`, byte +0x0D per 0x14 channel) → `lighting.c`; `telemetry_map_clamp`
    (linear map+clamp) → `util.c`.
  - **bike-state getters** → `app.c` (read the FSM object `0x20000029`+4):
    `bike_status_coarse_get`, `bike_state_is_standby`, `ble_lock_state_get`
    (0x28–0x37 pin-lock window), `ble_unlock_state_get`; plus `app_ctx_clear_field_328`
    (`0x200083A8`+0x328) and `clear_flag_00e5` (SMS flag `0x200000E5`).
  - **RTC helpers** → new `rtc.c`/`rtc.h`: `rtc_now_epoch_seconds` /
    `rtc_set_from_unix_time` — thin wrappers; the OEM delegates the calendar↔epoch
    date math to `FUN_08037F9C`/`FUN_08038110` and the register I/O to the RTC HAL,
    all kept as to-be-decoded externs (RTC handle `0x200099E4`).
  - **outbound-message enqueue** `maybe_enqueue_tx_message` → `ssp.c` (16-slot ×
    24-byte table @ `0x20007E14`, rolling handle @ `0x200000C8`); the verify caught
    the collision path advancing to the **next** slot (not a same-slot retry).
  - **BLE/state helpers** → `ble.c` (`count_active_2bit_groups` static,
    `sspm5_tx_timeout_cb`, `post_request_with_arg` @ reset-SM block `0x20006E44`)
    and `states.c` (`announce_records_reset` @ `0x20009360`, `display_announce_enter`,
    `display_timeout_timer_set`, `lock_poll_timer_arm`).
  - **Caught:** `ble_build_testmode_versions_blob` is called with `r0 = 0` from the
    0x5551 read yet stores `*out_crc` unconditionally (the OEM relies on the write
    to flash-alias address 0 being benign) — guarded with `if (out_crc)` to keep the
    observable behaviour without UB. **Deferred:** `config_persist_dual_bank`
    memcpy's 0xC0 stack bytes via `&stack0x00000000` (a variadic stack passthrough
    irreconcilable with the existing 4-arg callers) — left as a named extern.
    All literals/tables/sentinels re-read from the OEM binary; ARM-unsigned-char
    sentinels (0xFA slot, `(uint8_t)(state-0x28)` window) preserved.

- **Console/diag sourcing pass** — the three diagnostics leaves the console/BLE
  surfaces still called as named externs were decoded + adversarially verified (a
  decode→refute workflow, 10 agents, every format string + field offset re-read
  from memory and cross-checked against the live disassembly), then **all 3
  sourced** (build clean, `text 1996`):
  - `app_log_sink_enable` (`0x080298DC`) → `log.c`/`log.h`: a single byte store of
    1 to the "log to APP sink" enable flag at SRAM `0x200001D6` (read by the
    log-upload state machine). Labelled `g_log_app_sink_enable` in Ghidra.
  - `log_buffer_dump` (`0x080296B8`) → `log.c`/`log.h`: walks the SRAM ring
    (`LOG_CTRL`@`0x20037000` read→write cursor, wrap end `0x2004FC00`→payload
    `0x2003700C`), kicking the watchdog per byte, reassembling lines into a
    256-byte buffer (uint8_t index → over-long lines wrap, OEM behaviour). Each
    line's leading decimal is its epoch; when non-zero it is reformatted as
    `">DD/HH:MM:SS "` + the message from the first space on (via `rtc_epoch_to_calendar`
    + the OEM `strcspn`), else echoed raw `">%s"`. Reuses `rtc_calendar_t` +
    `rtc_epoch_to_calendar`, now **promoted from `rtc.c` into `rtc.h`** so both
    modules share them. Two helper callees named this pass: `parse_leading_decimal`
    (`0x08020DDC`, a `strtol(s,NULL,10)` wrapper) and `first_index_of_set`
    (`0x080216FC`, `strcspn`) — kept as externs (OEM ships its own copies).
  - `console_battery_dump` (`0x0804175C`) → `console.c`: the one-shot 1 s scheduler
    callback armed by `console_cmd_battery`; releases its slot then prints ~50 BMS
    telemetry fields from the session-context shadow (`CTXB`+0x3F2..+0x49E). Field
    encodings verified field-by-field: plain `%d` cells are s16; the three temps
    (+0x43C/+0x43E/+0x440) subtract 0xAAB (0.1 K → 0.1 °C); `I`/+0x3FE is ×10;
    HW_VER/FW_VER (+0x406/+0x408) print hi.lo (`%X.%02X`); FAULT/UBAT/FSR are raw
    u16. The PDOCP/+0x498 + PDSCP/+0x49A format strings live in a separate rodata
    island (`0x080507C8`/`0x080507D4`), not the contiguous `0x080542DC` block.
  Redundant `extern`s dropped from `ble.c`/`console.c` (both `#include "log.h"`).

- **Remaining named-leaf sourcing pass** — 14 leaves decoded + adversarially
  verified (decode→refute workflow, 45 agents, 3 lenses each on the trickier
  ones), **15 sourced** (build clean, `text 1996`); 2 deferred:
  - **SSP outbound pumps → `ssp.c`**: `tx_table_handle_in_use` (replaces its stub),
    `sspm_tx_queue_pump` (inter-module bus, the 16×0x18 table @ `0x20007E14` +
    timer pair @ `0x200000C8`, bus sender `sspm_bus_send_frame`@`0x0803A008`) and
    `ssp_ble_tx_queue_pump` (BLE SLIP queue @ `0x20008A40`, timer pair @
    `0x200000F0`, BLE-reset GPIOC pulse). Verify caught a candidate inventing a
    pointer var for the `0x20009044` (== `&queue[0x604]`) frame-staging address —
    it's an address immediate, passed directly.
  - **RTC date math → `rtc.c`**: `is_leap_year`/`days_in_year` + the two converters
    `rtc_calendar_to_epoch`(`0x08037F9C`)/`rtc_epoch_to_calendar`(`0x08038110`) +
    `rtc_fill_time_fields`(`0x080380A4`). The OEM magic-reciprocal divides are
    modelled as the plain `/60,/3600,/86400,/7,/100,/400` they implement (math
    lens hand-traced an epoch example); the days-in-month `[2][12]` table @
    `0x0804F340` materialised as `k_days_in_month`; the 2-digit-year
    `(char)year+'0'` byte-truncation quirk reproduced. HAL getters named
    `hal_rtc_get_time`/`hal_rtc_get_date`. rtc.c fully rebuilt to host all of it.
  - **misc**: `wwdg_apb_clk_disable` (clears RCC_APB1ENR.WWDGEN) → `watchdog.c`;
    `stc_gas_gauge_set_run` (STC3115 MODE reg bit0; `stc3115_read_reg`/`write_reg`)
    → `sensor.c`; `motor_get_timer_cb` (~12-field motor telemetry dump) → `console.c`;
    `shifter_mode_command_dispatch` (8-case OAD/mode dispatch driving G_STATE+4 +
    the update SM) → `shifter.c`; `region_speed_preset_table_load` (3 region preset
    tables materialised) → `app.c`.
  - **`config_persist_dual_bank` (the previously-deferred one) → `flash.c`**, now
    fully resolved: it takes 4 register words + a trailing **0xC0-byte by-value
    `struct boot_cfg_block`** (promoted to `flash.h`) and writes both flash banks
    (A `0x08008000` / B `0x0800C000`) via `flash_config_bank_write`. All 12
    call sites in `ble.c` (9) + `console.c` (3) rewritten from the old 4-arg +
    dead-`memcpy` placeholder to pass the payload struct; `settings_factory_reset`
    also sourced (→`app.c`) with the same call; main.c's local struct/extern
    folded into `flash.h`.
  - **Bonus bug fix**: `console_cmd_motorstatus` armed the motor poll on
    `SCHED_SLOT_TELEMETRY` (0x20000110) but the OEM (and `motor_get_timer_cb`) use
    `SCHED_SLOT_REC` (0x2000010E, verified at pool `0x08042F08`/`0x08040EB0`) —
    corrected.
  - ~~**Deferred** (status_process-class, not leaves): `enter_stop_mode` and
    `reboot_restart_task`~~ — **done**, see the "Sleep + reset/factory-reset SMs"
    entry at the top of this log. (The "unresolved ctx base" of the `0x08038A68`
    decompile turned out to be a false fall-through artifact: `reboot_restart_task`
    is a tiny 5-line NVIC-reset wrapper and the 6-state machine is the *adjacent*
    `factory_reset_sm_step` @ `0x08038A90`, whose base is a clean `ctx` parameter.)

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
  clean (gc'd, `text 1996`). **Charge/fault leaf cluster now SOURCED** (7 more
  funcs, +the 18-row charge-policy table @ OEM `0x0804F47C` materialized as
  `g_battery_charge_table`): `battery_charge_lookup` (SoC/temp/flags → rate+mode),
  `battery_charge_complete_watchdog` (2 s→10 s full-charge timeout),
  `battery_set_charge_mode` (writes app-ctx +0x341 via the `*G_BATWARE_UPD`
  pointer), `battery_telemetry_state_get` (G_BAT_STATE+3), `battery_fault_warning_report`
  (BMS fault/warn → state-flags `+0x3B8`, PC4 charger gate, `"BMS fault/warning
  0x%04X"`), `bus_queue_peek_status`, `status_clear_pulse_flag`. All DAT bases +
  the charge table + the two log strings re-read from the OEM bin (the decode
  agent's watchdog limit `-25511` was wrong → actual **-24999**).
  `scheduler_slot_get_remaining` sourced into `scheduler.c` (raw-counter twin of
  `scheduler_slot_is_idle`). Build clean, `text 1996`.
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
- **Placeholder-decode / naming pass** — the sourced modules called ~44 leaf
  helpers still left as `extern FUN_xxxxxxxx` placeholders. A fan-out workflow
  (5 groups + adversarial verify) decoded/classified/named them; all `FUN_`
  references across `src/` were then replaced with the real names (in source +
  Ghidra), build-neutral (`text 1996`). **Corrections the verifier caught:**
  `0x080398b8` is **`stc_gas_gauge_set_run`** (STC3115 MODE reg, set bit 0) — a
  prior session had mislabelled it `accel_enable` (the suspicion noted in
  `boot.md` is now confirmed); `0x080314a4`/`0x080314b4` are
  **`wwdg_apb_clk_disable`/`wwdg_apb_clk_enable`** (RCC_APB1ENR.WWDGEN bit 11 @
  `0x40023800`), **not** `flash_cache_disable`/`enable` (the regs are RCC, not
  FLASH — `update.c` calls them before blocking flash/stop). Reconciled
  collisions where the decode invented names already in use: `0x0803c5f0/5fc/608`
  = `obj_set_field34`/`obj_set_field38`/`led_channel3_set_brightness`
  (lighting.c); `0x0803c1c0` = `led_driver_enter_shipping_mode`, `0x0803b76c`
  = `display_request_clear`, `0x0803bec8` = `matrix_draw_icon`, `0x0803b2c4` =
  `display_send_init_cmd` (display.c); `0x08037aac` = `light_sensor_read_step`
  and `0x08037160` = `charge_level_adc_get` (states.c/lighting.c — kept);
  `0x08040350`/`0x08040368` are `gpio_pc0_is_low`/`gpio_pc1_is_low` (mask 1 = PC0,
  mask 2 = PC1). Newly named bespoke leaves include `battery_charge_lookup`
  (18-row charge-policy table @ `0x0804f47c`), `battery_fault_warning_report`,
  `battery_telemetry_state_get`, `bus_queue_peek_status`, `stc_read`,
  `rtc_set_from_unix_time`/`rtc_now_epoch_seconds`, `config_persist_dual_bank`,
  `watchdog_init`, `hw_version_lookup`, `announce_records_reset`,
  `display_announce_enter`, `display_timeout_timer_set`, `lock_poll_timer_arm`,
  `shifter_mode_command_dispatch`, `sspm5_tx_timeout_cb`,
  `ble_get_charge_plug_state`, `ble_build_testmode_versions_blob`,
  `ble_get_led_channel_state`, `telemetry_map_clamp`, `count_active_2bit_groups`,
  `post_request_with_arg`, `app_log_sink_enable`, `clear_flag_00e5`,
  `app_ctx_clear_field_328`, `lis3dh_powerdown`, `scheduler_slot_get_remaining`,
  `status_clear_pulse_flag`. `0x08024d2c` recognized as vendor `HAL_I2C_Mem_Write`.
  Only 6 `FUN_` refs remain in `src/` — all CubeF4 HAL I2C/`memset` vendor stubs.
  Bodies of the newly-named bespoke leaves are decoded (workflow output) but not
  yet sourced into their home modules — a per-module follow-up.
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
  by UART redirect: `bledebug`→UART7 (CC2642), `gsmdebug`→UART2 (modem AT), `bmsdebug`→Modbus.
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
  byte (`ctx+0x34C` / `ctx+0x34D`) so the main loop bridges the console to UART7
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
| `0x08040C6C` | ~30 | `console_cmd_bledebug`  | `src/console.c` | `bledebug`: UART7 sub-shell — set redirect ctx+0x34C |
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
