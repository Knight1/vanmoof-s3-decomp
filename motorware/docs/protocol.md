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
| **12** | 7 | `0x9017` flags, mode, `0x9018`/`0x9019`/`0x901A`, scaled `0x9066`(×625), scaled `0x91D6` | main status/telemetry block; **reading clears** flags `0x9017 &= 0xFFFC` |
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
