# Alarm / anti-theft subsystem

The alarm is **not a standalone module** in the firmware — it is the low half of
the central bike state machine `status_process` (`0x0802AAF8`, `src/states.c`).
`status_process` switches on the current bike-state byte `G_STATE[4]`
(`0x20000029 + 4`), and states **`0x00..0x05`** are the alarm states. Their names
come from `alarm_state_name` (`src/states.c`), which the state-change logger prints
as `BIKE_<name>`:

| State | Name | Meaning |
| --- | --- | --- |
| `0x00` | `ALARM_PRE_M1` | Armed / pre-alarm: watching for motion before the first alert |
| `0x01` | `ALARM_ACTIVE_M1_CNT` | First alert active, counting escalation |
| `0x02` | `ALARM_ACTIVE_M2` | Second (escalated) alert level |
| `0x03` | `ALARM_TRACKING_UNCONFIRMED` | Motion persisted → hand off to GSM tracking, not yet confirmed |
| `0x04` | `ALARM_TRACKING_CONFIRMED` | Backend/GSM confirmed the theft-tracking |
| `0x05` | `ALARM_BMS_REMOVED` | Battery pulled while armed |
| `0x23` | `ALARM_DELAY_ON` | Arming grace delay before `ALARM_PRE_M1` |

This is why the alarm looked "sporadic / referenced from other systems" — the logic
is threaded through the big `status_process` switch and its helpers rather than
sitting in an `alarm.c`.

## Trigger inputs

Two physical sensors feed the alarm, both polled inside `status_process`:

- **Wheel rotation — PC5** (`HAL_GPIO_ReadPin(GPIOC,0x20)`). Each edge logs
  `"Wheel trigger"` and sets the movement flag `G_CLK[0x89] = 1`. The last pin level
  is latched in `G_CLK[0xA1]`. (See `docs/hardware.md` → "User inputs".)
- **Accelerometer — LIS3DH INT1** (`lis3dh_int1_clear`, INT1 on PC3). Motion above
  the configured threshold logs `"Mems trigger"` and also sets `G_CLK[0x89]`.
- **BMS removed** — losing the battery module while armed drives state
  `ALARM_BMS_REMOVED` (`"App connect Leave, BMS alarm"`).

## Arming (`ALARM_PRE_M1`, state 0)

On entry the machine (re-)reads PC5 into `G_CLK[0xA1]`, clears the movement flag
`G_CLK[0x89]`, clears the LIS3DH INT1 latch, and arms a **5 s settle window**
(`scheduler_start(G_STATE[0x28], 5000, 0)`). When the window expires with no abort,
it clears the escalation state (`G_CLK[0x89]=0`, `G_CLK[0xA3]=0`, `G_CLK[0xA4]=0`),
advances to `ALARM_ACTIVE_M1_CNT` (state 1), stamps `ctx+0x310 = 1`, and **persists
the state record to EEPROM** (`save_state_record_to_eeprom`; logs `" ERROR Save
values"` on failure).

## Escalation & counters

Escalation is tracked in the `G_CLK` block (`0x200001D8`):

| Field | Meaning |
| --- | --- |
| `G_CLK[0x89]` | movement-detected flag (set by wheel/mems trigger) |
| `G_CLK[0xA1]` | last PC5 (wheel) level, for edge detection |
| `G_CLK[0xA3]` | escalation sub-state latch |
| `G_CLK[0xA4]` | **alarm count** — logged `"Alarm count %d"`; drives the M1→M2 escalation |

When a trigger fires while armed and the escalation is eligible, the machine calls
`set_mode_state_byte(0x12)` and, once motion persists, escalates to `ALARM_ACTIVE_M2`
and then to the tracking states. `"Go back to alarm %d"` marks a return to a lower
alarm state after a transient. The audible siren is driven through the audio path
(shared with the horn button); the tracking states hand off to the GSM modem
(`docs/modem.md`) for remote-tracking / `makeNoise`.

## Persistence

The alarm/lock state lives in the EEPROM **state record** (`docs/hardware.md` →
"EEPROM map"), not in flash config:

- `ctx+0x310` — alarm/bike state (record word 0), persisted on every state change.
- `ctx+0x317` — alarm **enable/disable** (record word 1, byte).
- The whole record is CRC-protected and mirrored (copies at EEPROM `0x00` / `0x40`).

## BLE control

The app controls the alarm over the defence GATT surface (`src/ble.c`,
`docs/ble-commands.md`):

- `CMD_BLE_DEFENCE_ALARM_STATE` — read/set the armed state.
- `CMD_BLE_DEFENCE_ALARM_SETTING` — alarm sensitivity / enable.
- `CMD_BLE_DEFENCE_LOCK_STATE` (`0x5521`) — lock/unlock, which arms/disarms.
- During `ALARM_PRE_M1` with a battery present, the firmware asks the app to unlock
  via SSP packet `0x5522` (`"Ask APP to unlock"`) before escalating.

## Where the code lives

- **State machine + escalation:** `status_process` (`0x0802AAF8`, `src/states.c`),
  switch cases `0x00..0x05`.
- **State names:** `alarm_state_name` (`src/states.c`).
- **Sensors:** PC5 wheel poll + LIS3DH INT1 (`src/states.c`, `src/lis3dh.c`).
- **Persistence:** `save_state_record_to_eeprom` (`src/app.c`, EEPROM in `src/eeprom.c`).
- **BLE:** defence commands in `src/ble.c`.

See also `docs/status-process.md`, `docs/state-machine.md`, `docs/hardware.md`.
