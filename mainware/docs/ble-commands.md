# mainware — BLE app-command map

The phone app controls the bike over BLE. The bleware MCU (CC2642) bridges the
GATT writes onto the inter-module bus; mainware receives them in
`ble_ssp_dispatch` → `ble_cmd_dispatch` (`0x08033970`), a big switch on the
16-bit command id. The `0x55xx` ids are the GATT characteristic writes (the id
== service + char index + 1); the low ids (`0x0A`..`0x123`) are internal /
provisioning commands.

`ble_cmd_dispatch(uint cmd, uint p2, uint8_t *payload)` is a ~2.5 KB switch
(named + documented, not sourced). The map below is read from its disassembly;
`ctx` = the app/session context, fields cross-referenced with the `show` dump
and `console.md`. Most app commands log, mutate one or more `ctx` fields,
`config_persist_dual_bank` or `save_state_record_to_eeprom`, `channel_notify_with_status`
(UI/sound feedback), and `ssp_ble_enqueue_tx_packet(cmd, …)` to ack the phone.

## Lock / power / alarm (the anti-theft core)

| cmd | action |
| --- | --- |
| `0xFA`  | **power on/off**: payload[0]=1 — from shipping (state 3/4) → exit shipping (state 0x0B, save record, state 0x30); from state 5 → power on (state 0x0E + mode 0x0F) |
| `0x5521` | **lock**: 1 = lock (state 0x20), 2 = unlock (reset buffers, state 0x30), 0 = if locked & lock-pin engaged → kick state 0x24 |
| `0x5523` | **lock/unlock state machine**: 0 = unlock (state 0x0E), 1 = lock (state 0x0E), 2 = state 0x01, 3 = state 0x02, 4 = state 0x04; persists the state record |
| `0x5524` | **alarm on/off** (ctx+0x317); arms a display request, saves the state record, acks |
| `0x5562` | **lock/alarm arm** — guarded state machine (checks bike state + lock-pin GPIO PC8): 1/2 → alarm-active (states 0x0E/0x07), 0 → lock or unlock-request (state 0x24 / 0x12) by pin state |
| `0x55C1` | **log-to-app** toggle (ctx+0x313); payload 3 → request state 0x17 (factory/test) |

## Region / speed / motor

| cmd | action |
| --- | --- |
| `0x5535` / `0x553c` | **region / speed mode** (ctx+0x109: 0=EU,1=US,2=JP,3=OFFROAD); `0x553c` also sets the region lock (ctx+0x144); config-persist + display request + ack |
| `0x5534` | **motor power level / assist** (ctx+0x3C9 1..4); sets the per-level ramp time ctx+0x354 (180/120/60/30) and the ride-change flag (ctx+0x3CB); `FUN_0803BEC8` |
| `0x5537` | **speed "moments"** — six per-region up/down speed points (ctx + region*6 + 0x10E..0x12A, scaled ×10); config-persist |
| `0x5536` | **shift gear** (ctx+0x3CC 1..4); increments the shift counter (ctx+0x338); `FUN_08028458` |
| `0x5538` | **transmission** auto/manual (ctx+0x108); config-persist |
| `0x5564` | **wheel size** (ctx+0x10B 24/28); toggles a GPIO + schedules a 1 s task; config-persist + ack |

## Audio / lights / display

| cmd | action |
| --- | --- |
| `0x5571` | **play notify/sound** — `channel_notify_with_status(payload[0])` |
| `0x5566` | **horn/sound select** (`FUN_08038EC4`, index < 7) + display request |
| `0x5574` | **horn file select** (ctx+0x318) + save record + ack |
| `0x5581` | **light mode** (ctx+0x10C: 0=auto,1=on,2=off); config-persist + display request |
| `0x5582` | **LED control** — three channels on/off via `FUN_0803C5FC`/`FUN_0803C5F0`/`FUN_0803C608` |
| `0x5584` | **dark/light threshold** (ctx+0x102) + config-persist |
| `0x5533` | **units** imperial/metric (ctx+0x10A) + config-persist + ack |

## Identity / config / backup code

| cmd | action |
| --- | --- |
| `0x5503` | **backup code / SOC** — payload `FF FF FF` clears it (ctx+0x100 = 0xFFFF, alarm off); else ctx+0x100 = `p0 + 10·p1 + 100·p2`, alarm on; config-persist + ack |
| `0x5572` | **backup-code/group set** (ctx+0xF4/0xF8/0xFC, the audio/group words; `FUN_08033914` = digit count) + config-persist + ack |
| `0x5531` | **set odometer/distance** (ctx+0x31C) + ack |
| `0x11A` | **provision BLE MAC + serial** (ctx+0x390..0x395 MAC; `snprintf` serial → ctx+0x398) |
| `0x105` | **bulk config write** (memcpy 0x100 bytes → ctx+0x1DC, set valid flag ctx+0x2DC) |
| `0x55A1` | set two flag bits (ctx+0xF0/0xF1) |

## Diagnostics / test / modem

| cmd | action |
| --- | --- |
| `0x55A2` | **testmode config A** — build a hw-CRC descriptor {1,10,1,0x100} and copy a 0x20-byte payload block |
| `0x55A3` | **testmode config B** — descriptor {0x0B,9,1,0x100} + 0x20-byte payload block |
| `0x5565` | **modem restart** (`FUN_0803CE08`, zero the SMS-info step) if payload 1 |
| `0x123`  | drive GPIO **PE5** (mask 0x20) high/low (BLE-chip reset line) |
| `0x5567` | `FUN_080381D0(payload u32)` |

## Internal / provisioning (low ids)

| cmd | action |
| --- | --- |
| `0x0A` | write ctx+0x388 (u32) | `0x0B` | write ctx+0x376 (u16) |
| `0x0C` | write motor params ctx+0x364/0x368/0x36C (u32) + 0x370 (u16) |
| `0x0D` | write ctx+0x372 (u32) | `0x10E` | `FUN_08029C0C(payload)` |
| `0x113` | write ctx+0x3E3 (u16); `FUN_0802A03C` | `0x118` | write ctx+0x38C (u32) |
| `0x119` | write ctx+0x3E2 (u8) | `0x11B` | write ctx+0x37C/0x380/0x384 (u32) |
| `0x122` | `FUN_0802E3D0(u16)` | default | unknown id → logged |

Each `0x55xx` write is acked back to the phone with
`ssp_ble_enqueue_tx_packet(cmd, len, payload, 0)`.

## Read / telemetry surface (`ble_read_request_dispatch`, `0x08034D20`)

The read twin — what the app **polls** to render bike state. Each GATT char-id
reads `ctx` field(s) (or a helper), packs a response, and acks via
`ssp_ble_enqueue_tx_packet(char_id, len, &resp, 0)`. Named + documented (a ~3.9 KB
packed-byte switch, not sourced). Most read ids mirror a write id from the table
above; the read-only ones are live telemetry.

| char id | reads / returns |
| --- | --- |
| `0x14`   | motor data block (ctx+0x378, Modbus poll to the motor) |
| `0x19`   | battery data block (ctx+0x3B2, Modbus poll to the BMS) |
| `0x5503` | backup-code-set flag (ctx+0x100 == 0xFFFF ? unset) |
| `0x5521` | **lock state** (`ble_lock_state_get`: 0 unlocked / 1 locked / 2 in PIN-lock) |
| `0x5523` | **unlock/alarm state** (`ble_unlock_state_get`, off alarm-mode ctx+0x310) |
| `0x5524` | alarm on/off (ctx+0x317) |
| `0x5531` | odometer / distance (ctx+0x31C) |
| `0x5532` | speed (ctx+0x3C2, scaled) |
| `0x5533` | units (ctx+0x10A) |
| `0x5534` | motor power level + ride-change (ctx+0x3C9/0x3CB) |
| `0x5535`/`0x553c` | region (ctx+0x109) + region lock (ctx+0x144) |
| `0x5536` | shift gear (ctx+0x3CC) |
| `0x5537` | speed "moments" (per-region up/down, scaled) |
| `0x5538` | transmission (ctx+0x108) |
| `0x5539`/`0x553a` | horn file / pedal speed (ctx+0x374 / 0x372) |
| `0x5541` | **battery summary** — SoC, temps (ctx+0x422/0x424), extbat (ctx+0x3D4/0x3DD/0x3DE/0x3E0/0x3E1) |
| `0x5542` | BMS serial/version (ctx+0x3D5..0x3D7) |
| `0x5543` | charger state (`FUN_08037160`) |
| `0x5544` | LiPo status index (ctx+0x3D0) |
| `0x5546`/`0x5547` | motor telemetry (ctx+0x36C / 0x36A) |
| `0x5548` | **motor error flags** (ctx+0x3A4..0x3AB, 8 B) if any set |
| `0x5549` | model / variant string (ctx+0x64A) |
| `0x554a` | **app fw version** (image header @ 0x08020004) — gated on state ≠ 3 |
| `0x554c`/`0x554d`/`0x554e`/`0x554f`/`0x5550` | version strings: ctx+0x388 / HW (`hw_version_lookup`) / modem (ctx+0x3E8) / shifter (ctx+0x336) / powerbank (ctx+0x408 + S/N) |
| `0x5551` | testmode blob (`FUN_0802A9DC`, 0x60 B) |
| `0x5561` | coarse bike status (`bike_status_coarse_get`) |
| `0x5562` | alarm armed? (`bike_state_is_standby`, state 0x0E) |
| `0x5563` | **error/status flags** (ctx+0x3B8, 8 B — the Diag/Error Flags words) |
| `0x5564` | wheel size (ctx+0x10B) |
| `0x5567` | tracking/modem field (`FUN_080380EC`) |
| `0x5568` | **button states** + lock pin (`FUN_08040350`/`FUN_08040368`, GPIOD PD2) |
| `0x5569` | supply OK? (`supply_voltage_read` ≥ 20000) |
| `0x5572` | backup-code group words (ctx+0xF4/0xF8/0xFC) |
| `0x5574`/`0x5581`/`0x5582`/`0x5584` | horn file / light mode / LED states / light-sensor (ctx+0x318 / 0x10C / `FUN_08037A68`×3 / ctx+0x102) |
| `0x55c1` | log-to-app (ctx+0x313), or 3 when state == 0x17 (DIAGNOSE) |
| default | unknown char id → logged |

The lock/alarm/status reads collapse the fine bike-state byte into coarse
app-facing enums (`ble_lock_state_get` / `ble_unlock_state_get` /
`bike_status_coarse_get` / `bike_state_is_standby`) — see `state-machine.md`.
