# mainware — bike lock / alarm state machine

The bike runs one top-level state machine. The current state is a single byte at
**SRAM `0x2000002D`** (`= 0x20000029 + 4`), read with `maybe_get_bike_state`
(`0x08029BA0`) and written with `maybe_set_state_if_unlocked` (`0x08029B88` — sets
the byte unless the current state is 6 or 7, i.e. shipping is sticky). The state
*codes* (0..0x3D) are the `alarm_state_name` table (`0x08032DF0`, sourced in
`states.c`); the per-state behaviour is driven each super-loop by `status_process`
(`0x0802AAF8`). The BLE/console commands are the *inputs* that request transitions;
the read side collapses the fine state into coarse app enums.

## Fine states (the `alarm_state_name` codes), grouped by phase

| phase | states (code = name) |
| --- | --- |
| **boot / ride** | `0x0D` INIT · `0x0C` RIDING_MODE · `0x0B` PLAY_FIRE · `0x13` CARDRIDGE_REMOVED |
| **standby / lock** | `0x0E` STANDBY · `0x11` SHOW_LOCK · `0x34` LOCK_PLAY_UNLOCK · `0x35` LOCK_PLAY_START · `0x36` LOCK_DIM_OFF · `0x37` LOCK_CLEAR · `0x38` LOCK_SETUP · `0x39` LOCK_PIC · `0x3A` LOCK_COUNT |
| **unlock** | `0x30` UNLOCK · `0x31` EXTRA_ALREADY_UNLOCKED · `0x32` UNLOCK_COUNT · `0x33` UNLOCK_COUNT_TIMEOUT · `0x24` TURN_ON |
| **PIN entry** | `0x26` PIN_START · `0x27` PIN_STUCK · `0x28`–`0x2A` PIN_1ST/2ND/3ND · `0x2B` PIN_CHECK · `0x2C` PIN_OK · `0x2D` PIN_SHOW_OK · `0x2E` PIN_NOK · `0x2F` PIN_NOK_SHOW |
| **alarm / anti-theft** | `0x00` ALARM_PRE_M1 · `0x01` ALARM_ACTIVE_M1_CNT · `0x02` ALARM_ACTIVE_M2 · `0x05` ALARM_BMS_REMOVED · `0x23` ALARM_DELAY_ON |
| **tracking** | `0x03` ALARM_TRACKING_UNCONFIRMED · `0x04` ALARM_TRACKING_CONFIRMED |
| **shipping** (sticky) | `0x06` SET_SHIPPING · `0x07` SHIPPING · `0x08` BIKE_SHIPPING_ACCIDENTAL_WAKE · `0x09` BIKE_SHIPPING_LIPOCHARGE · `0x0A` START_FROM_SHIPPING |
| **power / sleep** | `0x0F` CPU_STOP_MODE · `0x10` CPU_STOPPED · `0x12` AUTOWAKEUP · `0x25` LOW_SOC · `0x1F` PLAY_SHTDN · `0x20` PLAY_LOCK_SHTDN · `0x21` PLAY_LOCK_FROM_SLEEP · `0x22` PLAY_SHTDN_RDY |
| **charging** | `0x14` LIPOCHARGE · `0x15` CHARGING |
| **OTA (OAD)** | `0x19` OAD_UPDATE · `0x1A` OAD_FILE_TRF · `0x1B` OAD_FAILED · `0x1C` OAD_RX_SOUND · `0x1D` OAD_FINISH |
| **diag / factory** | `0x16` RESET · `0x17` DIAGNOSE · `0x18` DIAG_RDY · `0x1E` FACTORY_TEST |
| **misc** | `0x3B` COUNT_OFF · `0x3C` COUNT_CLEAR · `0x3D` FIND_MY_PLAY (Apple Find My) |

## Coarse app-facing views (what the phone reads)

The BLE read handlers collapse the fine state into small enums:

- **`bike_status_coarse_get`** (`0x08029BAC`, read by chars 0x5561 / 0x554x):
  `0` ready/standby (`0x0E`,`0x15`,`0x1B`,`0x1D`) · `1` riding (`0x0C`) ·
  `2` alarm/tracking (`0x03`,`0x04`) · `3` updating (`0x19`,`0x1A`; also `0x0D` if
  ctx+0x32C set) · `4` shipping (`0x06`,`0x07`) · `6` sleep (`0x0F`) · `7` other.
- **`ble_lock_state_get`** (`0x0802A8E8`, char 0x5521): `2` if state in `0x28..0x37`
  (PIN/lock sequence), else `bike_is_locked()` → `1`/`0`.
- **`ble_unlock_state_get`** (`0x0802A90C`, char 0x5523): off the alarm-mode byte
  `ctx+0x310` — `0x0B`→`bike_is_locked`, `2`→3, `3/4`→4, `0/1`→2, else 0.
- **`bike_state_is_standby`** (`0x08029B74`, char 0x5562): state == `0x0E`.
- **`bike_is_locked`** (`0x0802A8B0`): the lock GPIO **PC8** (mask 0x100) engaged,
  OR the software lock flag `ctx+0x312`, OR lock-state `ctx+0x340 == 1`.

## Transition inputs (commands → requested state)

From `ble_cmd_dispatch` (`docs/ble-commands.md`) and the console:

| input | → state(s) |
| --- | --- |
| BLE `0x5523` lock/unlock | 0/1 → `0x0E` STANDBY · 2 → `0x01` · 3 → `0x02` · 4 → `0x04` (alarm/tracking) |
| BLE `0x5521` lock | 1 → `0x20` PLAY_LOCK_SHTDN · 2 → `0x30` UNLOCK · 0 → `0x24` TURN_ON (if locked + pin) |
| BLE `0x5562` alarm-arm | 1/2 → `0x0E`/`0x07` · 0 → `0x24` TURN_ON or `0x12` AUTOWAKEUP (by pin) |
| BLE `0xFA` power | shipping-exit → `0x0B` → `0x30` UNLOCK · from `0x05` → `0x0E` STANDBY + mode 0x0F |
| BLE `0x55C1` / `0x5574` | → `0x17` DIAGNOSE / sound + display |
| console `setoad`, `batware` / BLE update | → `0x19` OAD_UPDATE |
| console `shipping` / `factory-shipping` | → `0x07` SHIPPING |
| console `batware` | → `0x19` |

`maybe_set_state_if_unlocked` refuses to overwrite states 6/7 (shipping), so a
bike in shipping mode ignores most transition requests until explicitly woken.
The alarm-mode persisted byte (`ctx+0x310`, EEPROM state record) is the durable
companion to the volatile state byte, set by the lock/unlock commands.
