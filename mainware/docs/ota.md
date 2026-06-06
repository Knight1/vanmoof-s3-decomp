# mainware — OTA orchestrator (`subsystem_update_sm`)

`subsystem_update_sm` (OEM `0x08031900`) is the bike's **firmware-update engine**.
One big state machine, ticked from the super-loop, that pulls a multi-file update
package over BLE (the phone forwards it from the cloud) and flashes it into
**every** subsystem on the bike:

| file in the package | target | type | flash dest | how it's flashed |
| --- | --- | --- | --- | --- |
| `mainware.bin` | this STM32F413 (self) | 2 | `0x08060000` (shadow, 0x40000) | shadow flash, then reboot into the bootloader (`NVICReset`) which swaps banks |
| `motorware.bin` | motor controller (TMS320 C28x) | 3 | `0x080A0000` | bus push to the motor (`maybe_enqueue_tx_message`) |
| `shifterware.bin` | e-Shifter (MM32F031) | 4 | `0x08010000` | bus update, gated on shifter power + Vbat |
| `batteryware.bin` | battery BMS (STM32L0) / powerbank | 5 | `0x080C0000` | Modbus to slave `0xAA`, Vbat-gated |
| `bleware.bin` | BLE co-processor (CC2642) | 1 | (ext) | OAD push to the BLE chip (`0x11b`/`0x11c`) |

The "type" column is the value `status_process`/this SM writes to `ctx[0x32C+i]`
when the bin's name matches in state 5; the state-0x16 commit switch dispatches on
it. **Now sourced as full C in `src/update.c`** (verified against the binary).

The package is a **PACK** container (see `pack_validate`, `docs/oad.md`): a header
with N "bins", each `{name, size, dest offset}`. The SM logs `"Number of bins %d"`,
`"Offset: 0x%08X"`, `"Size : 0x%08X"`, and per-bin `"%s %d bytes on 0x%08X"`.

## State map

The SM state is a byte at the head of the update-context block (one struct,
reached via several literal-pool aliases all holding the same RAM pointer; the
session/app context is `param_1`). Per-subsystem progress lives at
`ctx+0x32C..0x330` (`*(param_1+0x32c+file_index)`), the per-bin image table at
`+0x1C`/`0x40`-byte strides, the flash dest/src/size at block `+0x1A0..0x1A8`.

| state | name | action |
| --- | --- | --- |
| 1 | **ASK HEADER** | arm a 10 s `Ask_Header` timer, request the package header; on timeout → state 2 |
| 2 | **HEADER WAIT done** | reset counters, arm 5 s `fw_timeout_tmr`, ack `0x104` (request block), → 3 |
| 3 | **GET HEADER** | on reply parse the PACK header (`memcmp` magic, offset/size/bin-count); bad → testmode(3)+abort(0x15); ok → 0x15 path / next |
| 4 | **ASK BLOCK** | request the next 0x40-byte descriptor block (`0x104`), arm timer, → 6 |
| 5 | **SCAN FILES** | for each bin, `memcmp` its name against `mainware.bin`/`motorware.bin`/`shifterware.bin`/`batteryware.bin`/`bleware.bin`; set the dest region + size (shadow `0x40000`, motor `0x20000`, shifter `0x10000`, …) and jump to **state 7** (erase) for the first match |
| 6 | **RECV BLOCK** | copy the received 0x40-byte image descriptor into the per-bin table; loop to 4 until all bins read, then → 5 |
| 7 | **ERASE** | `flash_cache_disable` → `flash_erase(dest, size)` → `flash_cache_enable` + `watchdog_kick`; logs `"Erasing shadow flash %d Kb"`; → 8 |
| 8 | **DL WAIT** | spin until `download_chunks_pending_count()==0x80` (all 128 chunks buffered) → 9 |
| 9 | **ASK DATA** | ack `0x104`, arm 5 s timer, → 10 |
| 10 | **WRITE** | `flash_write(dest, src, 0xFF)` 0x100 B at a time, advance dest/src; on completion log `"Written %d bytes/sec"` → state 5 (next file); errors → `"Flash write error %d"` |
| 0xB–0xD | **SHIFTER push** | `0x110` OAD start to the shifter, `update_mode_request(4)`, `maybe_enqueue_tx_message(10,…)`; `"Start … update"` |
| 0xE | **SHIFTER wait** | poll `shifter_update_status_get()` (1/3 = done → 0x16, 2 = busy) |
| 0xF | **BMS arm** | poll `batteryware_update_status_get()`; on ready save the state record, → 0x10 |
| 0x10 | **BMS update** | drive the battery FW update over **Modbus slave 0xAA** (`modbus_bat_submit`: func 3 read-id, func 6/0x10 write), watchdog-pet loop, GPIO `0x1000`, save record |
| 0x11–0x13 | **BLE (bleware) OAD push** | `0x11b`/`0x11c` SSP image transfer to the CC2642, 15 s timeouts, progress vs `ctx+0x380/0x38c` |
| 0x14 | **ERROR** | testmode(7), → 0x15 |
| 0x15 | **CLEANUP** | release timers, log `"Stopped with ERROR"`/done, state → 0 (idle) |
| 0x16 | **FINALIZE / per-file dispatch** | the heart: a nested switch on each bin's target type, validating (`pack_validate`) and committing each subsystem — see below |

## State 0x16 — the per-target commit

After all files are flashed, state 0x16 walks the bin list and, by target type
(`ctx[0x32c+i]`, the value set in state 5), does the final commit + verify for
each (the exact mapping per `src/update.c`, verified against the binary):

- **type 0 (none/first):** for index 0 → abort with error (`testmode(4)`); else
  finish (`testmode`, state → 0).
- **type 1 (bleware):** advance the index and go to **state 0x11** (BLE OAD push,
  `0x11b`/`0x11c`).
- **type 2 (mainware self):** `pack_validate` the shadow image; once the arm-timer
  fires, save the state record, then **reboot**: `systick_delay(10)`, DSB, write
  `NVICReset` (`SCB_AIRCR = 0x05FA0004 | (AIRCR & 0x700)`) and spin forever — the
  muco bootloader swaps to the new bank on next boot
  (`"Update Main by reboot, wait for BMS shutdown"` / `"NVICReset"`).
- **type 3 (motorware):** `pack_validate`; compare the flashed version vs
  `ctx+0x388` (motor version) — match → done, mismatch → bump the index → state
  0xB. Retry-bounded.
- **type 4 (shifterware):** 5 s timer; `pack_validate`; compare vs `ctx+0x336`
  (shifter version) — match → done (0x16), mismatch → `shifter_update_request(2)`
  → state 0xE. Up to 4 retries, then `testmode(10)` abort.
- **type 5 (batteryware / powerbank):** wait Vbat > 0x61A9 (25 V) and powerbank
  present (`ctx+0x408`); `pack_validate`, compare vs `ctx+0x408`; match → done,
  mismatch → `batteryware_update_arm()` + `0x110` start → state 0xF. Vbat-too-low
  aborts (`"BMS update no vbat"`), up to 4 retries.

Each path logs the firmware-version compare (`"flash version: 0x%08X"`,
`"Timeout 0x%08X 0x%08X"`) and, on success per subsystem, `"Update Ok"`.

## Notable details

- **Self-update is reboot-driven**: mainware can't overwrite its running bank, so
  it stages `mainware.bin` into shadow flash (`0x08060000`) and issues a CPU
  reset (`SCB_AIRCR` = `0x05FA0004 | (AIRCR & 0x700)`, then spin); the **muco
  bootloader** validates + bank-swaps on next boot. The
  `"Update Main by reboot, wait for BMS shutdown"` log shows it sequences the BMS
  power-down first. The update-SM control struct lives at SRAM `0x20000760`
  (`g_update_sm`); its scheduler-slot handles at `0x20000079` (`g_update_slots`).
- **Gating on power**: shifter and BMS updates refuse to start without adequate
  Vbat (`"Shifter update no vbat (%d)"`, `"BMS update no vbat"`) — a flat battery
  can't be left mid-flash.
- **Each subsystem verifies after flashing** by reading back its version and
  comparing to the package's, retrying a bounded number of times before aborting
  to `testmode_command_dispatch(<err>)`.
- **Helpers** (named this pass): `memcmp` (`0x08020E60`),
  `flash_cache_disable`/`enable` (`0x080314A4`/`B4`, FLASH_ACR bit 11 around
  erase), `download_chunks_pending_count` (`0x0803FA98`),
  `shifter_update_status_get` (`0x08029540`), `batteryware_update_status_get`
  (`0x0803F444`), `shifter_update_request` (`0x08029528`),
  `batteryware_update_set_pending` (`0x0803F420`), `bus_rx_byte_locked`
  (`0x080363EC`). Flash/PACK leaves are in `flash.c` (`docs/` `oad.md`).

The BLE/console entry points that *start* this SM are `setoad`/`batware` and the
`0x5510` OAD service (`docs/ble-commands.md`, `docs/oad.md`); the OAD ext-flash
slot map and PACK format are in `docs/oad.md`.
