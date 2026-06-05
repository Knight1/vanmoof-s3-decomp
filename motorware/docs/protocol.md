# Protocol — motorware ↔ mainware

How the motor controller is addressed, updated, and queried. Facts tagged
**(verified)** are proven (header bytes, CRCs, the boot stream, or the VanMoof
tool/`ver` console output); **(inferred)** are reasoned but not yet line-proven
in the motorware image.

## Identity

* **Module type byte `0xA1`** **(verified)** — header `+0x05` region; constant
  across `S.0.00.15` and `S.0.00.22` (only the patch byte changes). Sits with
  batteryware `0xAA`, powerbankware `0xB2`.
* **Version display** **(verified)** — the VanMoof tool prints the motor app as
  `v0.00.16` with build `( 03 2021 00:48:35)` size `61720` — i.e. the patch is
  shown in **hex** (`0x16` = 22), matching `motorware_S.0.00.22.bin` exactly.
* MCU reported loosely as "F2806" by the tool; it is an F2805x (F28054F).

## Firmware update path **(verified from the tool + image)**

The motor app is delivered as the **TI C28x boot-ROM data stream** wrapped in
the standard VanMoof header (see `container.md`), and uploaded over **YMODEM**:

* tool/console commands: `em` = *erase motor app*, `um` = *upload motor app
  (Y-modem)*, `motorupdate` = *update F2806 CPU*.
* The motor MCU's resident boot/flash kernel receives the YMODEM stream, parses
  the boot blocks `[size][dstHi][dstLo][data]`, and programs flash; on reset the
  boot ROM enters flash at `BEGIN 0x3F7FFE` → `_c_int00 0x3F4799`.
* A `MotorPcb CRC ok` check is reported after update.

So the boot-stream codec in `tools/bootstream.py` *is* the update format: it
can unwrap, verify (CRC + byte-exact round-trip), and reconstruct exactly what
the YMODEM upload carries.

## Runtime link to mainware — **SLIP + CRC-16 over SCI-A (verified)**

Traced in the image (`src/comm.c` reconstructs it). The motor↔main link is
**not** Modbus-RTU framing — it is **SLIP** (RFC 1055) framing with a
**CRC-16/Modbus** integrity trailer, point-to-point, **no slave-address byte**
(`0xA1` appears nowhere as an address compare). Stack, bottom-up:

| Layer | function | what it does |
| --- | --- | --- |
| PHY | **SCI-A** (`SciaRegs` `0x7050`) | the bus; HAL stores its handle at comm-object `+0xC0`, `sci_tx_byte` reads it back from there |
| TX queue | `sci_tx_byte` `0x3F4294` | push 1 byte into a **64-deep software ring** (`s_tx_buf` `0x94C0`, head `0x94BB`, count `0x94BD`), then kick TX (`sub_3F3F6D`) |
| Framing | `slip_tx_frame` `0x3F3310` / RX SM `0x3F33E6` | SLIP encode/decode: `C0`=END, `DB`=ESC, `DB DC`=END, `DB DD`=ESC (RX state @`0x95B8`: idle/in-frame/after-ESC) |
| Integrity | `modbus_crc16` `0x3F4B2C` / `_byte` `0x3F4B20` | CRC-16 poly **`0xA001`**, **init `0xFFFF`**, appended lo-then-hi before the closing `C0` |

Wire frame: `C0 | payload(stuffed) | crc_lo,crc_hi (stuffed) | C0`, CRC computed
over the *unstuffed* payload.

This 0xA001 CRC is the same polynomial the S3 Modbus modules use, but here it
rides **SLIP frames**, not Modbus silent-interval framing — so motorware's link
is its own thing, distinct from the battery/shifter Modbus bus. (The mainware
BLE/tool surface still treats the motor as a module: `motorstatus`,
`motorupdate` via YMODEM, the `0x0404` sleep handshake, and CSV telemetry
**MOTORCURRENT / MOTORTMP / DRIVERTMP / SPEED**.)

## Command layer (verified)

`sub_3F3472` (the link-service routine, called from the main control function
`sub_3EE894`) decodes one SLIP frame and dispatches on the **opcode** = the
2nd payload byte. Decoded payload (after de-stuffing, CRC stripped):

```
byte 0   type     (responses use 0x02)
byte 1   opcode    5 / 6 / 7
byte 2.. operands  (register id, address, value …)
```

| opcode | handler | meaning |
| --- | --- | --- |
| **6** | `sub_3EE47C(reg)` | **read register** `reg` → value (IQ-scaled to int) |
| **7** | `sub_3EE50E(addr,val)` | **write** (addr in `[2..3]`, value in `[4..6]`; indexes the L3 struct — 48 L3 accesses) |
| **5** | `sub_3F345C(sub)` | opcode-5 sub-command dispatch |

Read/write build a response `{0x02, 0x05, value…}` and send it via
`slip_tx_frame`. An RX return code of 2 sets a status bit: `*(0x9017) |= 2`.

### Status/fault word `0x9017` (the "errors" mainware reads)

Register 12's first word is the L3 fault/status word at `0x9017`; reading it
clears the two acked low bits. Each bit is set at a verified detection point
(the test preceding `0x9017 |= bit`) — see `src/motor_state.h`:

Cross-referenced to the mainware user-facing motor error codes
(Knight1/VanMooof-Module `ERRORS.md`, errors 45–53) — the descriptions confirm
the detection mechanisms found in the image:

| bit | name | set when | → mainware error |
| --- | --- | --- | --- |
| `0x0001` | FAULT_INIT_CFG_A | startup HAL/config check fails | — |
| `0x0002` | FAULT_RX_ERR | SLIP receive error | 22 MOTOR_COMMUNICATION |
| `0x0004` | STATUS_TOGGLE | recurring status (mirrored to byte `0x9019`) | — |
| `0x0008` | FAULT_MEAS_0008 | ISR: computed value on `0x9068` > 0 | — |
| `0x0010` | FAULT_THRESH_0010 | ISR: value ≤ 100 | — |
| `0x0020` | FAULT_DRV_FAULT | **DRV8301** status pin == 0 | 46 MOTOR_OVER_CURRENT / 45 CABLE |
| `0x0040` | FAULT_DRV_OCTW | DRV8301 status pin == 0 | 46 / 52 TORQUE_SENSOR_FAIL |
| `0x0100` | FAULT_INIT_CFG_B | cfg invalid (`0x95A4`==0 && `0x901E`<5) | 53 MOTOR_NOT_READY [~] |
| `0x0200` | FAULT_CURRENT_OFFSET | current-sense **offset** outside window | **49 MOTOR_CURRENT_ERR** |
| `0x0400` | FAULT_VOLTAGE_OFFSET | voltage **offset** outside window | **50 MOTOR_VOLTAGE_ERR** |
| `0x0800` | FAULT_THRESH_0800 | ISR: value ≤ a limit | — |
| `0x1000` | FAULT_OVERTEMP | 3-threshold range check | **51 MOTOR_DERATING** (high temp) |
| `0x2000` | STATUS_TIMEOUT | periodic/timeout (ePWM4 tick reset) | — |
| `0x8000` | STATUS_RUN_REQ | run requested (write-reg 20) | — |

The match is tight: the image checks the **current/voltage sense offsets** against
windows (`sub_3EE2FD`) — exactly errors 49 ("current offset … deviated") and 50
("voltage offset … incorrect"); the 3-threshold check (`sub_3EE842`) is the
temperature **derating** (51); and error 46 names the **DRV8301** gate driver,
confirming the `0x0020`/`0x0040` digital-input faults are its status pins. Errors
48 (CONTROLLER_ERROR) / 53 (NOT_READY) come from the estimator-state checks
(`EST_getState`/`EST_isMotorIdentified`).

### Reliable delivery (verified)

Responses aren't sent inline — they're **enqueued** by `sub_3F32C9` into an
**8-entry × 13-word ring at `0x9440`** (seq counter `0x95B9`); each entry is
`[key, status=5, count, data…]` (count ≤ 8 words, else error `0xFD`/`0xFF` when
full). The queue is drained as opcode-5 frames and **retransmitted until the
master acks** — an inbound **opcode 5** (`sub_3F345C`) walks the ring and clears
the matching entry. So opcode 5 is the data/ack channel; opcodes 6/7 are the
request channel.

## Register map (verified from `sub_3EE47C` / `sub_3EE50E`)

**Read — opcode 6** (`sub_3EE47C(id)`; response enqueued with the id + N words):

| id | words | source (L3) | meaning |
| --- | --- | --- | --- |
| **10** | 2 | `0x90A2`–`0x90A5` | firmware version / identity (the `DEADBEEF`+version region; returns patch `0x16`=22) |
| **11** | 1 | digital inputs 37 & 32 via `sub_3F3B0C` | status bits (bit0 ← in 37, bit1 ← in 32) |
| **12** | 7 | `0x9017` flags, mode (calls **`EST_getSpeed_krpm`** = FAST motor speed), `0x9018`/`0x9019`/`0x901A`, scaled `0x9066`(×625), scaled `0x91D6` | main status/telemetry block; **reading clears** flags `0x9017 &= 0xFFFC` |
| **13** | 2 | scaled `0x906A`, scaled `0x905C` | two IQ measurements (×125 ≫17 / ×… — engineering-unit conversion) |

**Write — opcode 7** (`sub_3EE50E(id)`; value = `frame[0..1]` big-endian, then
int→IQ via `sub_3F4BD9` + an IQmath scale chain):

| id | scale const | effect |
| --- | --- | --- |
| **20** | — | enable/mode flags: `frame[0].bit0` → set `0x9017\|=0x8000` + action `sub_3F40E8`; `frame[1].bit7` → `0x9013` |
| **21** | `0x42C8` (100.0) | setpoint (÷100 → IQ) |
| **22** | — | ack / no-op |
| **23** | `0x42C8` (100.0) | setpoint (÷100 → IQ) |
| **24** | `0x4120` (10.0) | setpoint (÷10 → IQ) |
| **25** | `0x4120` (10.0) | setpoint → `0x95A4` |

The setpoints (21/23/24/25) are the drive commands (target current / speed /
boost) the master writes; the read registers (10–13) are the telemetry it logs
(MOTORCURRENT / MOTORTMP / DRIVERTMP / SPEED). Exact per-id engineering units
and the remaining `0x9000`+ field names follow from the control-loop pass.

### Open items

1. **L3 field semantics** — finish naming the `0x9000`+ struct fields the
   registers touch (ties into the control-loop analysis).
2. **SCI-C** (`0x7770`, HAL `+0xD2`) — role (debug / GSM passthrough?).
3. **eCAN** (`0x6000`, 5 base loads) — telemetry or a second control channel?
