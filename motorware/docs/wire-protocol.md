# Talking to mainware as the motor — wire protocol

How to put bytes on the SCI-A bus so mainware believes a (healthy, turning)
motor is present — for a motor simulator / hardware-in-the-loop rig, or to
decode a real capture. Everything here is from the decompiled image
(`src/comm.c`, `src/registers.c`); the framing + CRC are exact, the response
field offsets are reconstructed (confirm them with a capture — see §6).

Build/decode frames with **`tools/motor_sim.py`**.

## 1. Physical layer

* **SCI-A** UART (TTL), the dedicated mainware↔motor link (`SciaRegs 0x7050`;
  the motor's HAL stores it at object `+0xC0`).
* 8-N-1. **Baud rate**: set through the SCI handle (not a literal in the image);
  **confirm from a capture** — common VanMoof inter-module value is 115200.

## 2. Framing — SLIP (RFC 1055)

A frame is byte-oriented:

```
C0  <payload bytes, stuffed>  <crc_lo crc_hi, stuffed>  C0
```

* `C0` (END) delimits frames (one at each end).
* Byte-stuffing inside the frame: `C0` → `DB DC`, `DB` → `DB DD`.

## 3. Integrity — CRC-16/Modbus

`CRC-16` with poly **`0xA001`**, init **`0xFFFF`**, computed over the
**unstuffed payload bytes** (not the delimiters), appended **little end first**
(`crc & 0xFF`, then `crc >> 8`) *before* stuffing and the closing `C0`.

## 4. Payload structure

```
request  (mainware -> motor):  [type][opcode][operands...]
response (motor -> mainware):  [0x02][0x07][reg][16-bit fields, big-endian][bytes...]
```

| opcode | meaning |
| --- | --- |
| 6 | read register (operand = register id at byte[3]) |
| 7 | write register (operand = id + value) |
| 5 | ack — clears a pending queued response |

Reads/telemetry are delivered as **reliable** responses: the motor queues them
and **retransmits until mainware acks** (an inbound opcode-5 frame). So a sim
both **pushes** telemetry and **acks** mainware's commands.

## 5. Make mainware think the motor is turning

The telemetry mainware watches is **register 12** — the status + speed block.
Its fields (`read_register(12)`):

| field | meaning | for "turning, healthy" |
| --- | --- | --- |
| status | `0x9017` fault word | **`0x0000`** (no faults) |
| **speed** | FAST-estimated motor speed (`EST_getSpeed_krpm`) | **non-zero** |
| byte18/19/1A | status bytes | 0 |
| meas1/meas2 | scaled measurements | plausible non-zero |

Forge it:

```bash
python3 tools/motor_sim.py                      # prints a ready "turning" frame
```

```python
from tools import motor_sim as m
frame = m.build_reg12(speed=300, status=0x0000)  # speed!=0, no faults
# -> C0 02 07 0C 00 00 01 2C 00 00 00 00 00 00 00 BC 49 C0
```

Decoded payload: `02 07 0c | 00 00 | 01 2c | 00 00 00 | 00 00 | 00 00`
= type 2, resp 7, **reg 12**, status `0x0000`, **speed `0x012C`=300**, …

**Operating loop for the sim:**
1. Periodically transmit the register-12 "turning" frame (and register-13
   measurements if polled).
2. When mainware sends an **opcode-5 ack**, stop retransmitting that entry.
3. Answer mainware's **opcode-6 reads** (e.g. reg 10 = version) and **ack its
   opcode-7 writes** (the drive setpoints) with `[0x02,0x05,echo]`.
4. Keep `status = 0` (no `0x9017` faults) or mainware raises motor errors
   45–53 (see `protocol.md`): non-zero speed + clean status = "turning".

> Tuning note: `speed` is the FAST estimate in the motor's own scale (krpm /
> IQ). Match the magnitude a real motor reports at your target wheel speed —
> capture a genuine session and read it back (§6) to calibrate.

## 6. Validate against a real capture (recommended)

Because the response field offsets are reconstructed through two firmware
layers, confirm them on real hardware before relying on them:

```bash
# paste the captured C0…C0 bytes (hex) to de-stuff + CRC-check + show payload:
python3 tools/motor_sim.py decode  c0 02 07 0c 00 00 01 2c ... c0
```

Capture a few mainware↔motor exchanges, decode them, and check the register-12
payload matches §5 (and read the real `speed` scale). Then `build_reg12()`
will produce byte-identical frames you can replay/modify. This closes the one
soft spot in the spec empirically.

## Safety

This is for a bench rig / simulator on a bus you own. Feeding fabricated
"healthy" telemetry to a controller driving a real motor can mask genuine
faults — don't do it on a bike you ride.
