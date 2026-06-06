# mainware — battery / BMS Modbus driver (`battery.c`)

`src/battery.c` is mainware's **battery subsystem**: a Modbus-RTU master that
talks to the battery's BMS (the STM32L072 running `batteryware`) over the
inter-module bus. It polls cell/charge telemetry, drives the pack through its
detect → bring-up → charge → off lifecycle, and pushes BMS firmware updates.

**Sourced** as faithful C (18 functions), fan-out + adversarially verified
against the live disassembly. The register map and framing **cross-validate the
`batteryware` decomp** (the slave side).

## Modbus framing (master → BMS slave 0xAA)

PDUs are built on the stack and submitted to the bus ring via `modbus_bat_submit`
→ the shared queue. The frame is `{ slave, func, reg(BE u16), … , [0x84]=count }`:

| builder | func | frame |
| --- | --- | --- |
| `bms_modbus_read(reg, count)` | **3** (read holding regs) | `{0xAA, 3, reg, …, count}` |
| `bms_modbus_write(reg, value)` | **6** (write single reg) | `{0xAA, 6, reg, val_lo, val_hi, …, 1}` |

`bat_modbus_master_step` is the byte-level **RTU master state machine**: it
serialises the request (each byte via `bus_tx_enqueue_byte`, accumulating the
**CRC-16** with `bus_crc16_update`, appending `bus_crc16_get()` lo/hi), then
receives the response byte-by-byte (`bus_rx_byte_locked`) — echo check, function
check (top bit = exception → error), byte-count, data into the frame's `[0x85+]`
response area, and a final CRC verify. `bat_modbus_txn_pump` drives one
transaction (send → poll → dispatch → error → bus-reset); `modbus_bat_service_step`
ticks it each super-loop and on repeated errors flushes + re-inits the link
(`" ERR Modbus Bat"` / `" ERR MBB flush"` / `" ERR MBB reset"`, fault flag
`0x80000`). The CRC-16 (poly 0xA001) + framing match the `batteryware` slave.

## Telemetry register cache → app context

`bms_telemetry_unpack` parses a func-3 response and writes each register as a
**big-endian u16** into the app-context cache: `reg N → *(uint16_t*)(g_app_ctx +
0x3F2 + N*2)`. `battery_request_telemetry` polls the two blocks `regs 0..0x30`
and `regs 0x47..0x56` — covering the SOC/voltage/current/temperature summary and
the per-cell voltages (the batteryware map puts cells 1–10 at regs 27–36).
Special-cased registers: **reg 5** → a config byte (`ctx+0x315`), **reg 0xB** →
the battery serial/id (`ctx+0x408`, logged on change). With the BMS-debug flag
(`ctx+0x34F`) set, every register value is also dumped to the console.

## BMS lifecycle (`battery_telemetry_step`, state at `G_BAT_STATE[3]`)

A ~16-state machine on the BMS substate byte:

| state | name / action |
| --- | --- |
| 0 | **ask ID** — reinit, `bms_modbus_read(0,1)`; `"BMS ask ID"` |
| 1 | **detect** — PC10 low → `"No BMS detected"`; PC10 high → reset-pulse PB5 + `"BMS Pulse"` |
| 2 | **confirm** — PC10 high → `"BMS detected"` |
| 3 | **wait ID** — on reply → bring-up (6); timeout → `"BMS: no ID"` |
| 4 | **sleep check** — PD1 → `"BMS in sleep mode"` |
| 6/7 | **bring-up** — `"BMS set discharge"`, enable discharge (write reg 8/9), motor reset; arm telemetry |
| 8 | **charge/discharge edge** — PC4 charger sense → `"BMS: No CHG"` / `"BMS: No DSG"` |
| 9 | **reset arm** — `"ADC Vbat %d"`; gate on Vbat ≥ 0x61A9 (25 V) → `"BMS oke %d"` |
| 10/0xB | **reset pulse** — `"BMS Resetting %d"` → drain RX → `"BMS start again"` |
| 0xC | **steady telemetry poll** — periodic `bms_modbus_read(3,4)`; SOC-missing `"BMS: No SOC"`; FAULT-pin (PD1) edge → `" ERR/CLR battery FAULT PIN"` |
| 0xD/0xE | **re-arm** |
| 0xF | **off** — `"BMS off"`, `bms_modbus_write(1,0)`, PC10 → next state 5/4 |

`battery_state_process` is the pack presence/charge/error classifier;
`battery_charge_display_step` computes the SOC % (current-limit + `Set power
state to %s`) and pushes it to the display. `battery_on_detect_step` (PC10) and
`battery_substate_advance` are called from `status_process`;
`batteryware_update_arm`/`set_pending`/`status_get` are the OTA hooks driven by
`subsystem_update_sm` (`docs/ota.md`).

## Hardware

| signal | pin | role |
| --- | --- | --- |
| BMS present | **PC10** (GPIOC `0x40020800`, 0x400) | pack-inserted detect |
| charger sense | **PC4** (GPIOC, 0x10) | charging vs discharging |
| sense / FAULT | **PD1** (GPIOD `0x40020C00`, 2) | BMS sleep / fault-pin |
| BMS reset | **PB5** (GPIOB `0x40020400`, 0x20) | reset/power pulse |
| motor reset | **PB10/PB9** (GPIOB, 0x400/0x200) | motor reset during bring-up |
| — | **PD13** (GPIOD, 0x2000) | driven low at discharge-enable |

SRAM: `g_bat_modbus_ctx` @ `0x20006E90` (Modbus ctx: master-SM state, RX
counter, bus handle `+0xE74`, txn-SM state `+0xE78`, frame `+0xE7C`), the BMS
state object `G_BAT_STATE` @ `0x200000E7` (substate `[3]`, timer slots
`[0]/[5]/[7]`, retry `[6]`, mode latch `[8]`), and the batteryware-update record
@ `0x20008A00` (charger shadow `+0x3E`). Telemetry lands in the app context
(`g_app_ctx` @ `0x200083A8`, cache at `+0x3F2`).
