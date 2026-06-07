# mainware — shifter / drivetrain Modbus master (`shifter.c`)

`src/shifter.c` is the **master side of the flagship shifterware**: a Modbus-RTU
master to the eShifter / MT-shifter module (Modbus slave **0x20**) over the
**USART3** inter-module bus (9600 baud, 8N1). It is the structural twin of
`battery.c` (the BMS Modbus master on the *other* bus) — same frame-ring queue
abstraction, same byte-level RTU engine shape — but a distinct bus, context and
register map.

**Sourced** as faithful, behaviour-equivalent C (37 functions), transcribed by a
fan-out workflow and **adversarially verified** against the live disassembly.
Verification caught a systematic hazard worth recording: the per-case log-format
strings in the two big state machines are packed in a dense flash literal pool,
and several were transcribed one slot off. Every literal was re-resolved
byte-for-byte from the OEM image (`file_offset = vaddr − 0x08020000`) before the
final assembly, so each format string matches its argument list exactly.

## Bus + register conventions

| | |
| --- | --- |
| Transport | USART3 (`g_usart3_handle` @ SRAM `0x20009824` → USART3 `0x40004800`), 9600 baud |
| Framing | Modbus-RTU; CRC-16 reflected, poly **0xA001** (`shifter_crc_*`) |
| Slave | **0x20** (eShifter / MT shifter) |
| Functions | 3 = read holding regs, 6 = write single, 0x10 = write multiple |
| Rail / enable | GPIOD **PD7** (`0x80`) 24 V rail; GPIOE **PE14** (`0x4000`) enable |

The PDU frame is the same 0x108-byte generic layout battery.c uses:
`[0]`=slave, `[1]`=func, `[2..3]`=register (native u16, transmitted big-endian),
`[4..]`=request data, `[0x84]`=byte count, `[0x85+]`=response data,
`[0x105]`=response payload length, `[0x106]`=Modbus-exception flag.

### Shifter register map (as exercised by the master)

| reg | dir | meaning |
| --- | --- | --- |
| 1 | r | ID / manufacture probe (HW-version word → ctx +0x520: 0x200 std, 0x201 MT) |
| 2 | r/w | current gear (the gear actuator writes here, func 6) |
| 3,4,5,6 | r | status block (sequential poll); reg 6 = FW version |
| 0xE,0xF | r | extended status (MT) |
| 0x14 | w | MT commit/apply |
| 0x16,0x30 | r | MT telemetry block |
| 0x30 | r/w | MT zeropos read / `0xFDE8` write |
| 0x40 | w | MT zeropos save (func 0x10, four 32-bit words) |
| 0x50 | w | MT mode/current (`0xFD28`, `100`) |
| 0x80 | w | reset / boot-into-app (OTA) |
| 0x81 | r | CRC/erase answer poll (OTA) |
| 0x82 | w | firmware data block, func 0x10, 0x20 bytes/block (OTA) |
| 0x95 | w | erase command (OTA) |

## SRAM model

| symbol | addr | role |
| --- | --- | --- |
| `g_shift_modbus_ctx` | `0x20005E3C` | master byte-SM state `[0]`, RX counter `[2]`, ring handle `[0xE74]`, txn-pump state `[0xE78]`, PDU frame `[0xE7C]`, func `[0xE7D]` |
| `g_modbus_crc` | `0x2000008C` | u16 CRC-16 accumulator; `+2` = queue/RX-timeout scheduler slot |
| `g_shift_rx_timeout_slot` | `0x2000008E` | RX-flush one-shot slot (== `g_modbus_crc+2`) |
| `g_shifter_sm` | `0x2000001C` | control-SM slot-record: `+1` step, `+3/4/5/7/9/0xA` scheduler slots, `+6` substate, `+8` current gear |
| `g_shifter_ctx` | `0x2000019C` | subsystem state: `+0` update step, `+1` commit, `+2` result, `+4` active flag, `+8` staged pack header, `+0x14` image len, `+0x30` prog offset, `+0x34/0x35` retries, `+0x37` seq-poll step, `+0x38` link-fail, `+0x39` comm-fail |
| `g_bus_ring_slots` | `0x20006E00` | shared 4-entry frame-ring descriptor pool (battery + shifter draw from it) |
| `g_usart3_rings` | `0x2000094C` | USART3 byte rings: `+0xC1C` TX, `+0xC20` RX |
| `G_APP_CTX` | `0x200083A8` | the session context = `g_app_state.ctx_sub`; the SMs' `ctx` param and the func-3 reply-register destination (reg N → `+0x51E + N*2`) |

## Layers

1. **Frame-ring queue** (`bus_queue_init/push/pop/reset/peek`) — generic 16-byte
   descriptors from the shared pool; `modbus_shift_submit` enqueues, the pump
   dequeues. These are the same primitives battery.c references as externs.
2. **RTU engine** (`shifter_modbus_rtu_step`) — the byte-level master TX/RX state
   machine (build + CRC-frame a func 3/6/0x10 request, validate the echo,
   exception bit and trailing CRC) plus the CRC-16 helpers and the locked USART3
   byte I/O (`shifter_uart_tx_byte/_buf/_rx_byte`, RXNEIE/TXEIE masked around the
   rings). An RX-flush watchdog (`shift_rx_flush_timeout_cb`, 0x5DC ticks) forces
   the SM to the flush state if a reply stalls.
3. **Transaction pump** (`shifter_modbus_pump`) — pops a queued frame, drives the
   RTU engine, and on a func-3 completion calls `shifter_response_unpack` to
   decode the reply registers into the session context (+0x520..). The
   `shiftdebug` console flag (ctx `+0x34E`) streams each register as
   `"MBS 0x%04X 0x%02X"`.
4. **Link monitor** (`modbus_shifter_link_monitor`) — the per-super-loop entry
   (called from `main`). Gates on the link-enable flags + uptime + active flag,
   pumps the transaction SM, tracks the consecutive-comm-fail counter (flush +
   state-flag `0x1000` after 5 failures), then always runs the control and update
   SMs.
5. **Drivetrain control SM** (`shifter_control_step`) — ~18 states: power-up, the
   ID/HW-version handshake, the **auto-shift** logic (bike speed ctx `+0x3c2`
   against per-region gear up/down threshold tables at `+region*6 + {0x10e..}/{0x126..}`,
   region = ctx `+0x109`), the manual shift-button path (button state via
   `FUN_08040350`), and the MT **zeropos calibration** sequence. The gear
   actuator is `shifter_send_gear` (func 6, reg 2). `shifter_seq_status_poll_step`
   is the 18-state register sweep used during bring-up.
6. **OTA update SM** (`shifter_update_sm_step`) — ~20 states: probe, `pack_validate`
   the staged image at flash `0x08010000`, stream it in 0x20-byte blocks via
   func 0x10 to reg 0x82, poll the CRC/erase answer (ctx `+0x620`,
   `0x2A6`=pending), reset + read back version, then persist the state record to
   EEPROM. `shifter_firmware_update_step` is the pre-check gate that arms it
   (type byte 0xC1↔HW 0x200, 0xC9↔HW 0x201).
7. **Accessors + console dumps** — `shifter_sm_get_step` / `set_step_3/10/13`,
   `shifter_get_active_flag`, `shifter_update_request`, `shifter_update_status_get`,
   and the `shifterstatus` callbacks `shifterstatus_dump_v200` (std) /
   `shifterstatus_dump_v201` (MT, with the per-gear position loop).

## Notes

- The `s*` console commands (`swritedata`, `sreadreg`, `swritereg`, `shiftware`,
  `shiftdebug`, `shifterstatus`, `shiftresetcounter`) live in `console.c`; they
  inject through `modbus_shift_submit`, which had **no definition** until this
  module — `shifter.c` now provides it.
- `pack_validate` is the project's 2-arg form (`flash.h`): it derives the image
  length from the pack header and writes a 0x28-byte (10-word) normalised header
  to `out_hdr`, so `shifter_firmware_update_step` reserves `uint32_t hdr[10]`.
- ARM `char` is unsigned: the scheduler "no slot" sentinel `0xFA` (Ghidra
  `== -6`) is compared as `SCHED_SLOT_NONE`.
- The module gc-sections out of the linked image (the build's `main` is a weak
  stub), so the ELF stays `text 1996`; `shifter.o` is 9551 B of code.
