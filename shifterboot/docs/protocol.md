# shifterboot — Modbus RTU protocol

Reverse-engineered from `main`'s phase-2 dispatcher (decomp-c at
`src/main.c`). Confirms and extends the inter-module bus model from
`hardware.md`.

## Bus parameters

| Parameter | Value | Where |
| --- | --- | --- |
| Wire format | Modbus RTU (8-N-1) | `boot_init_usart1` |
| Baud rate | 9600 | main calls `boot_init_usart1(75 << 7)` |
| Slave address | `0x20` | `main` rejects any other addr at the dispatcher head |
| Frame timeout | none observable | shifterboot accumulates bytes via `USART1_IRQHandler` and lets `main` decide on each loop iteration whether the buffer holds a complete frame |

The dispatcher does not implement the canonical Modbus RTU 3.5-character
idle-gap framing. Instead it length-discriminates: 8-byte frames are
treated as func-0x03 / 0x06 PDUs, 45-byte frames as func-0x10 OTA-stream
PDUs.

## Dispatch table

Dispatched on `(func_code, sub_id)`, where `func_code = frame[1]` and
`sub_id = frame[3]`. The two-byte addressing is canonical Modbus
"register address low byte", which VanMoof repurposes as a sub-command
selector.

| func | sub-id | Wire size | Behaviour |
| --- | --- | --- | --- |
| `0x03` | `0x01` | 8 B | "Ping" — reply with template B (constant `0x0200` register value) |
| `0x03` | `0x81` | 8 B | **Apply image**: `image_verify_crc()`, reply with the result (0/1/2), latch a `NVIC_SystemReset` on next loop iteration |
| `0x03` | `0x95` | 8 B | **Erase slot 1** (clear OTA staging), echo back the inbound frame as ack |
| `0x03` | other | 8 B | Reply with template A (constant `0x0001` register value) |
| `0x06` | * | 8 B | Same dispatch table as `0x03` (same byte layout) |
| `0x10` | `0x82` | 45 B | **OTA stream**: 32 B image chunk at `frame[11..42]`, big-endian stream offset at `frame[7..10]` |

Any other `(func, sub_id)` combination → silently drop (reset `RX_IDX`).

## Frame layouts

### Short PDU (func 0x03 / 0x06) — 8 bytes

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | slave addr | must be `0x20` |
| 1 | function code | `0x03` or `0x06` |
| 2 | addr_hi | (ignored by the dispatcher) |
| 3 | addr_lo = **sub_id** | selects the handler |
| 4..5 | qty / value | used by cmd-0x81 to derive the reply byte at offset 3 |
| 6..7 | CRC | RTU CRC16 over bytes 0..5 |

### OTA stream PDU (func 0x10) — 45 bytes

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 1 | slave addr (`0x20`) |
| 1 | 1 | function code (`0x10`) |
| 2..3 | 2 | start register addr — `addr_lo` (frame[3]) must be `0x82` |
| 4..5 | 2 | qty of registers (16 = 32 bytes of payload, canonical for the chunk size used) |
| 6 | 1 | byte count (= 36? — not consumed by shifterboot; only the payload offset matters) |
| 7..10 | 4 | **stream offset, big-endian** — position within the image at which this chunk should land |
| 11..42 | 32 | image bytes |
| 43..44 | 2 | CRC over bytes 0..42 |

The first chunk (stream_offset == 0) carries the 40-byte image header
starting at `frame[11]`. The dispatcher pulls the `length` field
(header offset +12 → `frame[23..26]`) and uses it to compute the total
chunk count + final-chunk remainder.

## Replies

| Cause | Reply size | Contents |
| --- | --- | --- |
| cmd 0x01 (ping) | 7 B | template B verbatim: `20 03 02 02 00 05 23` |
| cmd 0x81 (apply) | 7 B | `slave, func, ((BE16(qty) & 0x7F) << 1), 0, image_status, CRC_lo, CRC_hi` |
| cmd 0x95 (erase slot 1) | 8 B | echo back the inbound frame |
| cmd unknown (func 0x03/0x06) | 7 B | template A verbatim: `20 03 02 00 01 C5 83` |
| cmd 0x82 (OTA chunk) | 8 B | `frame[0..5]` + fresh CRC |
| CRC mismatch / other drop | none | RX_IDX is just reset |

The two response templates are pre-built in main's stack locals at boot,
copied in from pool words `0x080004DC..0x080004EB`:

- **Template A** (sp+104): `20 03 02 00 01 C5 83 00` — "I'm at register
  value 0x0001" (CRC computed for that frame is `0x83C5`)
- **Template B** (sp+112): `20 03 02 02 00 05 23 00` — "I'm at register
  value 0x0200" (CRC `0x2305`)

The `0x0200` constant in template B is most likely a version-style
identifier (5.12 decimal? or 0x200 = 512), used by mainware to
distinguish "shifterboot is alive" from "shifterware is alive". TBD —
needs mainware-side dispatcher decomp to confirm.

## OTA cycle (master perspective)

```
master                shifterboot
   |                       |
   |--- 0x95 erase  ------>|  flash_erase_pages(slot 1, 12)
   |<-- ack ---------------|
   |                       |
   |--- 0x82 chunk[0] ---->|  ota_program_chunk(slot1+0,  frame, 32)
   |<-- ack ---------------|
   |--- 0x82 chunk[1] ---->|  ota_program_chunk(slot1+32, frame, 32)
   |<-- ack ---------------|
   |       ...             |
   |--- 0x82 chunk[N-1] -->|  ota_program_chunk(slot1+(N-1)*32, frame, remainder)
   |<-- ack ---------------|
   |                       |
   |--- 0x81 apply ------->|  image_verify_crc() → result
   |<-- result ------------|
   |                       |  apply_pending = 1
   |                       |  NVIC_SystemReset()  ← end-of-loop
   |                       |
   |                       === cold-boot ===
   |                       |
   |                       |  phase 1: slot1.crc == slot2.crc
   |                       |       → erase slot 1
   |                       |  slot 2 valid, slot 1 invalid
   |                       |       → boot_app(slot 2)
```

## Cold-boot probe: `0x1B` byte

Before entering phase 2, the cold-boot path waits 250 ms, then samples
`MODBUS_RX_BUF[0]`. If it equals `0x1B`, the loader erases slot 2 (the
active image) — a recovery hook the master can trigger by sending any
frame whose first byte is `0x1B` during the 250 ms window. The
mainware-side semantic is unclear from shifterboot alone; possibly a
"clear active image, fall back to OTA-server mode" command.

`0x1B` is **not** a Modbus slave address used by shifterboot — the
dispatcher rejects anything other than `0x20`. So the `0x1B` byte is
probably a frame addressed at a different module on the bus that
shifterboot happens to be listening to during the 250 ms window.

## Outstanding protocol questions

- **What does the cmd-0x81 reply byte at offset 3 mean?**
  Computed as `((BE16(frame[4..5]) & 0x7F) << 1)`. The master sets
  `frame[4..5]` to a 16-bit value (Modbus qty-of-registers field), and
  the shifter echoes back the low 7 bits doubled. Possibly a slot/lane
  selector the master uses to match the reply with a queued request.
- **What is `0x0200` (template B's data value)?**
  Likely a version-style identifier; would need to be cross-checked
  against mainware's bootloader-vs-app probing code.
- **What does the `0x1B` cold-boot byte mean to mainware?**
  Probably a "force into Modbus listening mode" command — the loader's
  response is to erase slot 2, leaving both slots invalid → loop
  enters phase 2 with nothing to boot.
