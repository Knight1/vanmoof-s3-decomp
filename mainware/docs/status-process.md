# mainware — `status_process`, the per-state behaviour engine

`status_process` (OEM `0x0802AAF8`, ~14 KB — the largest function in the image)
is the bike's **brain**. It is called every super-loop and, dispatching on the
top-level bike-state byte (the `alarm_state_name` codes — see
`docs/state-machine.md`), runs the behaviour for the current state: read the
sensors, drive the lights/sound, manage charge & sleep, run the alarm / lock /
PIN flow, kick off tracking, and finalize OTA/diag with a reboot. Where the
BLE/console commands (`ble-commands.md`, `console.md`) are the *inputs* and the
state byte is the *position*, **`status_process` is the logic that moves between
states and acts in each one.**

**Now SOURCED as full faithful C → `src/states.c`** (alongside `alarm_state_name`).
The decompiler renders the whole function once its prototype is fixed to the
single context param (`void status_process(uint8_t *ctx)`); the ~2300-line body
is a common prologue followed by a 62-case `switch` on the bike-state byte
`G_STATE[4]` (cases `0..0x3D` = the `alarm_state_name` codes). It was transcribed
region-by-region (9 agents over the case ranges), assembled, and **adversarially
verified** against the live disassembly (the verify pass caught 3 slips — a
deref-vs-address argument in case 9 and two copy-pasted log strings in cases
0x1b/0x26 — now fixed). Build clean (gc-sectioned, `text 1996`).

Model: the dozens of `DAT_*` literal aliases the decompiler created collapse to a
few real bases — `ctx` (the param), `G_STATE` (`signed char *`@`0x20000029`, state
byte `[4]`, per-state scheduler-slot handles at `[0x14..0x3E]`), `G_CLK`
(`@0x200001D8`, the clocking / PIN-attempt / odometer aux block) and `g_log_func`.
ARM's unsigned `char` means the OEM `== -6` / `== -0x12` slot sentinels require
`signed char` for those two bases. The per-state log vocabulary below names every
branch; the call surface is the catalogue of actions.

## Sensors it reads each tick

- **Accelerometer = LIS3DH** (ST 3-axis): `"LIS3DH high sense"` / `"LIS3DH low
  sense"` — switches sensitivity between standby (high, theft detect) and ride.
  Feeds `"Mems trigger"` (motion while armed) and the alarm escalation.
- **Wheel sensor**: `"Wheel trigger"` / `"Wheel move"` — rotation while locked.
- **Buttons**: `FUN_08040350`/`FUN_08040368` (button state) + lock pin (PC8 /
  `bike_is_locked`); `"Double click"`, `"Button horn: off"`, `"USER Button
  reset: off"`.
- **Battery / cartridge presence**: `"Battery detected"` / `"Battery removed!"`
  / `"Cardridge is removed!"` / `"External battery removed"`; BMS poll via
  `bms_modbus_read` (Modbus 0xAA), `"Low BMS"`.
- **Charger / supply**: `"Charger detected Lights Off"`, supply voltage.

## Behaviour domains (from the per-state log vocabulary)

**Alarm / lock / anti-theft**
`"Locked"`, `"Kick lock locked"` / `"in standby"` / `"timeout"`,
`"Alarm count %d"`, `"Go back to alarm %d"`, `"Mems trigger"`, `"Wheel
trigger"`, `"SMS: Tracking active"` → starts the modem tracking SM
(`modem_sim_state_machine`, `docs/modem.md`). The unlock paths: `"Ask APP to
unlock"`, `"Auto wake request BLE unlock"`, `"Auto wake do unlock"`.

**PIN / backup code**
`"Start PIN state machine at %d attempts"`, `"3 times failed now wait %d
seconds"` (lockout via the scheduler), `"No backupcode"` / `"No backup code"`,
`"Horn stuck cannot use backup code"`. Drives the `0x26..0x2F` PIN states.

**Power / charge / sleep**
`"Charging liPo %d%%"` (+ `"not possible"`), `"Int LiPo decide: CPU stop"` /
`"… 24h"`, `"End of charge"` / `"End intLiPo"`, `"Wait 2 minutes before
evaluating"`, `"LiPo charged to at least %d%%. Going back to sleep"`, `"Sleep
timer elapsed"`, `"Set BLE sleep"` / `"BLE standby mode"`, `"Wake from
shipping"`. CPU-stop entry via `set_mode_state_byte` + the scheduler.

**Lights** (the `Lights P*` display programs)
`"Lights P1"`/`P1a`/`P2a`/`P2c`/`P3`/`P5`/`P6`/`PC`, `"Light P03"`/`P04`,
`"Force Lights Off"`, `"Charger detected Lights OFF"`. Driven through the LED
channels (`FUN_0803C5FC`/`5F0`/`608`) + `channel_notify_*`.

**Motor / shifter**
`"Shifter on"` / `"re-Shifter on"` / `"SHifter on"`, `"MT Shifter calibrate"` /
`"Shifter calibrate"` (`modbus_shift_submit`), `"Restore power level %d"`,
`"No MOTOR SLEEP MODE from motor 0"`, `"Start-GSM: riding"`.

**Diagnostics / OTA finalize**
`"Diag ok"` / `"Diag fail"`, `"OAD took too long"`, `"Update ok, Reboot"` →
`"NVICReset"` (commit a flashed update by rebooting into the bootloader),
`"Update fail, Reboot on key"`, `"Appcon NVICReset"`. Ties into
`subsystem_update_sm` (`docs/ota.md`) and `reboot_restart_task`.

**Errors**
`"Error Flags: %d"` / `"Error Flags: None"`, `"Possible error 21 recovered"` /
`"Possible error 21. Check again"`, `"ERR MS overflow"`, the SSP-enqueue
failures `"ERROR SSPM/SSPM2/SSPM3/SSPM4 place"`.

**Misc / lifecycle**
`"First time use"`, `"No kicklock coil"`, `"USER bike on"` / `"… with powerbank
in d[ock]"`, `"BIKE %s"`, `"SOC %d saved %d"`, `"Got %d"`.

## Key callees (the actions it invokes)

State model: `maybe_get_bike_state` / `maybe_set_state_if_unlocked` /
`set_mode_state_byte` / `alarm_state_name`. Feedback: `channel_notify_emit` /
`channel_notify_with_status` / `announce_mark`. Subsystems: `bms_modbus_read`,
`modbus_shift_submit`, `modem_info_ready`, `bike_is_locked`,
`batteryware_update_set_pending`. Persistence/lifecycle:
`save_state_record_to_eeprom`, `reboot_restart_task`, `testmode_command_dispatch`,
`console_cmd_logout`, `scheduler_*`, `watchdog_kick`, `ssp_ble_enqueue_tx_packet`,
`maybe_enqueue_tx_message`. **42 of its per-state sub-handlers are now named**
(see the "named" table in `progress.md`): e.g. `locked_state_step`,
`power_assist_gear_step`, `diagnostics_run_step`, `internal_lipo_charge_step`,
`enter_stop_mode`, `system_reset`, the shifter-SM steps, `state_flags_set`/
`clear`/`test`, the LIS3DH helpers (`lis3dh_int1_clear`/`powerdown`/
`config_motion_int`), `light_sensor_read_step`, `charge_level_adc_get`,
`led_driver_*_shipping_mode`, `battery_on_detect_step`, `bms_write_reg8_and_poll`,
`telemetry_datalog_emit`.

## Where it sits

```
super-loop ─tick─► status_process  (this — per-state behaviour)
                      ├─ reads: LIS3DH accel, wheel, buttons, BMS, charger
                      ├─ drives: lights, sound, motor/shifter, modem tracking
                      ├─ moves the state byte (maybe_set_state_if_unlocked)
                      └─ finalizes OTA/diag (NVICReset / reboot_restart_task)
inputs ─► ble_cmd_dispatch / console (request transitions)
state  ─► 0x2000002D byte = alarm_state_name codes (docs/state-machine.md)
```
