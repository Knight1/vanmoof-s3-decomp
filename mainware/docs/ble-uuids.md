# mainware — how the BLE UUIDs work

This explains the GATT UUID scheme the phone app uses to talk to the bike, and
how those 128-bit UUIDs collapse into the 16-bit command ids that mainware
dispatches (`ble-commands.md`). The UUID *table* physically lives in **bleware**
(the CC2642 BLE co-processor) — mainware never sees a 128-bit UUID, only the
16-bit short id that bleware forwards over the inter-module bus. This doc ties
the two halves together.

## The two-layer UUID model (standard BLE, VanMoof base)

Bluetooth LE lets a vendor define its own 128-bit UUIDs but still address them
compactly. VanMoof uses the usual trick: **one 128-bit base UUID with a 16-bit
"short" spliced into it.** The base is

```
6ACC0000-E631-4069-944D-B8CA7598AD50
     ^^^^
     bytes [2..3] = the 16-bit short id
```

Every service and every characteristic is this base with its short written into
bytes `[2..3]`. So short `0x5520` is the full UUID
`6ACC5520-E631-4069-944D-B8CA7598AD50`. This is exactly how the Bluetooth SIG
embeds 16-bit UUIDs in the standard base `0000xxxx-0000-1000-8000-00805F9B34FB`
— VanMoof just swapped in their own vendor base, so the whole attribute database
is "private" but addressed with two bytes internally.

## Service numbering

bleware's GATT config holds an **11-entry service table** (in
`bleware`, flash `0x0002A2F8`; each entry `{u16 short_id, u16 char_count,
u32 items_ptr}`). The service shorts are on `0x__0` boundaries:

| service short | full UUID | role (from the command map) |
| --- | --- | --- |
| `0x5500` | `6ACC5500-…AD50` | **backoffice / auth** (factory + secure channel) |
| `0x5510` | `6ACC5510-…AD50` | **OAD / firmware update** (opaque handler) |
| `0x5520` | `6ACC5520-…AD50` | **lock / alarm / power** (anti-theft core) |
| `0x5530` | `6ACC5530-…AD50` | **ride config** (region, speed, gears, units, wheel) |
| `0x5540` | `6ACC5540-…AD50` | **telemetry / status** (battery, motor, versions, errors) |
| `0x5560` | `6ACC5560-…AD50` | **alarm-arm / buttons / modem** |
| `0x5570` | `6ACC5570-…AD50` | **sound / backup code** |
| `0x5580` | `6ACC5580-…AD50` | **lights** |
| `0x5590` | `6ACC5590-…AD50` | (reserved / sparse) |
| `0x55A0` | `6ACC55A0-…AD50` | **factory test** |
| `0x55C0` | `6ACC55C0-…AD50` | **log readout** (opaque handler) |

## Characteristic numbering — the `svc + idx + 1` rule

Inside a service, characteristics are numbered **sequentially from `svc + 1`**:
the 0-based characteristic index `idx` has short id `svc + idx + 1`. So service
`0x5520` (lock/alarm) exposes `0x5521` (char 0), `0x5522` (char 1), `0x5523`
(char 2)… each a full UUID `6ACC552x-…AD50`.

The key identity that makes the whole system legible:

> **the characteristic short id == the inter-module command id == the value
> mainware dispatches on.**

When the phone writes characteristic `6ACC5521-…AD50`, bleware's central write
handler (`xs3_gatt_process_write_event`) computes `cmd = svc + idx + 1 = 0x5521`
and publishes that id + payload onto the bus. mainware receives it in
`ble_cmd_dispatch` (`0x08033970`) and hits `case 0x5521:` (lock). Reads work the
same way through `ble_read_request_dispatch` (`0x08034D20`). That is why
`ble-commands.md` can be keyed purely on the 16-bit ids — they *are* the
characteristic UUIDs minus the constant base.

```
phone app                 bleware (CC2642)                 mainware (STM32F413)
---------                 ----------------                 --------------------
write 6ACC5521-…AD50  →   (svc 0x5520, idx 0)
       payload               cmd = 0x5520+0+1 = 0x5521  →  ble_cmd_dispatch case 0x5521
                             forward over SSP/Modbus bus      → lock state machine
notify 6ACC5521-…AD50 ←   pack response             ←       ssp_ble_enqueue_tx_packet(0x5521,…)
```

To go the other way — from a UUID you sniffed to what it does:
`svc = short & 0xFFF0`, `idx = (short & 0x000F) - 1`, then look up `short` in
`ble-commands.md`.

## The auth gate (why a raw write usually does nothing)

Two characteristics in the backoffice service are special and are **not**
plain bus bridges:

- **`0x5502`** (`6ACC5502-…AD50`) — the **authentication handshake**. The app
  writes a 20-byte blob; bleware derives a 16-byte session key from a 4-byte
  seed inside it and stores it on the connection. Every other service's write
  callback first checks that a valid session key exists (per-characteristic
  "require session key" property bit). Without a successful `0x5502` handshake,
  writes to `0x5521`, `0x5523`, … are dropped *in bleware* and never reach
  mainware.
- **`0x5505`** (`6ACC5505-…AD50`) — the **backoffice** request/response channel
  (write *and* notify on the same characteristic), used for factory/secure
  operations rather than the simple command bridge.

So for bench testing with nRF Connect / `bluetoothctl`: subscribe to the
characteristic you care about, complete the `0x5502` handshake first, then write
to e.g. `6ACC5521-…AD50` to drive lock/unlock.

## Where each half is implemented

| concern | firmware | symbol / location |
| --- | --- | --- |
| 128-bit base + service/char tables | bleware | service table `0x0002A2F8`, items per service |
| central write routing, `svc+idx+1` | bleware | `xs3_gatt_process_write_event` `0x00004DB0` |
| auth handshake (`0x5502`) | bleware | `auth_derive_session_key` `0x00018B1C` |
| **command handlers (per short id)** | **mainware** | **`ble_cmd_dispatch` `0x08033970`** |
| **read/telemetry handlers** | **mainware** | **`ble_read_request_dispatch` `0x08034D20`** |

The per-short-id action map is in `ble-commands.md`; the lock/alarm states those
commands drive are in `state-machine.md`. The 128-bit table itself is bleware's
to own (a separate decomp target) — this doc only records the scheme so the
mainware command ids read as the UUIDs they are.
