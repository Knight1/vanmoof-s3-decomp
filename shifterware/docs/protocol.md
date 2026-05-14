# Bus protocol — VanMoof S3 inter-module Modbus link

The shifter talks to the bike's main module over a Modbus RTU half-
duplex link on **USART1, 9600 baud, 8-N-1**. This file documents
everything the decomp has revealed about the frame layouts, the CRC,
the command codes, and the RX/TX paths in the firmware.

## Wire framing

Modbus RTU with the standard CRC-16:

- **Polynomial**: `0xA001` (reversed `0x8005`)
- **Initial value**: `0xFFFF`
- **Byte order on wire**: CRC low byte then CRC high byte (Modbus
  convention)
- **Frame boundary**: inter-byte idle gap, tracked by an end-of-frame
  countdown driven by SysTick (`modbus_tick` decrement, threshold
  `0x00249F00` ticks)

The shifter's bus address is **`0x20`**. The RX FSM rejects any frame
whose first byte differs.

### Frame layouts

The shifter accepts two PDU lengths. Both are validated end-to-end
by the same CRC-16; the dispatcher reads `cmd` (byte 3) and
`len` (byte 1) verbatim from whichever buffer was filled.

**Short frame (8 bytes total)** — used for everything except OTA
payload writes:

| Byte | Field |
| ---- | ----- |
| 0 | `0x20` slave address |
| 1 | `len` (passed to dispatcher; pattern is "PDU length minus header"; observed values 3, 6, 0x0F) |
| 2 | TBD (cherry-picked by some case handlers; e.g. `cmd_5c_write3` reads `G_5C_REGS[0..2]` from offsets [2,4,5]) |
| 3 | `cmd` (function code; dispatched on) |
| 4 | TBD |
| 5 | TBD (used as a payload byte by cmd 0x5A) |
| 6 | CRC low |
| 7 | CRC high |

**Long frame (45 bytes total / 0x2D)** — used by OTA firmware-page
writes (cmd `0x82` with `len == 0x10`, accepted only after the
`G_RX_FRAME_MODE` byte has been switched to 1 via cmd `0x95`):

| Byte | Field |
| ---- | ----- |
| 0 | `0x20` slave address |
| 1 | `len` (0x0F observed) |
| 2..2A | TBD payload bytes (16 B of program data plus addressing info) |
| 2B | CRC low (over bytes 0..2A) |
| 2C | CRC high |

In both cases the CRC is computed over all bytes preceding it
(`modbus_crc16_compute(buf, N-2)`).

## RX path

1. **`USART1_IRQHandler`** (`uart.c`, OEM @ `0x0800450C`) reads one
   byte from USART1's RDR on each RX-ready interrupt and appends it
   to `G_RX_SCRATCH` (RAM `0x200001B2`) at index `G_RX_HEAD`
   (`0x200000E4`). Caps at 45 bytes; further bytes are silently
   dropped. Resets `G_RX_WAIT_CTR` (`0x200000DC`) to 0 on every
   byte, restarting the end-of-frame timer.
2. **`modbus_tick`** (`modbus.c`, OEM @ `0x080044DC`) is called
   periodically from an ISR (likely SysTick). It decrements
   `MODBUS_TICK_CTR` (`0x200000C4`) if non-zero — this is the
   inter-byte countdown used elsewhere on the bus.
3. **`modbus_rx_poll`** (`modbus_dispatch.c`, OEM @ `0x08003EDA`)
   is called from the main super-loop. It:
   - Returns early if `G_RX_HEAD == 0` (no bytes yet).
   - Drops the frame if the first byte isn't `0x20`.
   - In short-frame mode (`G_RX_FRAME_MODE == 0`):
     - waits while `G_RX_HEAD < 8` and ticks the wait counter;
     - on full frame, copies the 8 bytes into `G_RX_BUF`, CRC-checks,
       sets `G_REQ_PENDING`, calls `modbus_dispatch_pdu(cmd, len)`.
   - In long-frame mode (`G_RX_FRAME_MODE == 1`):
     - same shape but with threshold 45 and `G_LONG_BUF`;
     - on either timeout or CRC fail, exits long-frame mode and
       resets the OTA staging pointers (`G_OTA_WRITE_PTR` ←
       `0x08001800`).
4. **`modbus_dispatch_pdu`** (`modbus_dispatch.c`, OEM @ `0x08003C9A`)
   runs the switch documented below, then — regardless of which case
   matched — calls `modbus_reply_passthrough()` whenever
   `len == 6 || len == 0x0F`, and finally clears `G_REQ_PENDING`.

## TX path

- **`uart1_send_byte`** (`uart.c`, OEM @ `0x0800371E`) writes one
  byte to TDR and spins on the TX-ready flag.
- **`modbus_send_bytes`** (`modbus.c`, OEM @ `0x0800373A`) loops
  `uart1_send_byte` over a byte range.
- **`modbus_crc16_compute`** (`modbus.c`, OEM @ `0x0800378C`)
  computes the CRC-16 (poly `0xA001`, init `0xFFFF`) over `len`
  bytes and stores the result into the pair of bytes at
  `0x200000E7`/`E8`.
- **`modbus_tx_finalize`** (`modbus.c`, OEM @ `0x08003756`)
  transmits `len` bytes from `MODBUS_TX_BUF` (`0x200001A9`) and,
  iff `len == 7` AND `G_IMG_OK_FLAG == 1`, then writes
  `SCB->AIRCR = 0x05FA0004` to fire a SYSRESETREQ so shifterboot
  can install the freshly-validated OTA image.
- **`modbus_reply_passthrough`** (`modbus.c`, OEM @ `0x080037CC`)
  builds an 8-byte reply by copying the first 6 bytes of the
  inbound PDU into the TX buffer, appending the CRC, and calling
  `modbus_tx_finalize(8)`. Used for echo-style responses.
- **`report_image_status`** (`image.c`, OEM @ `0x08003A86`) builds
  a 7-byte status PDU from scattered RAM state and calls
  `modbus_tx_finalize(7)` — the only 7-byte transmit, hence the
  trigger condition for the post-OTA reset above.

## Command codes (`cmd` byte = inbound PDU[3])

Dispatched by `modbus_dispatch_pdu`. The GCC compiler emits the
switch as a `__gnu_thumb1_case_uqi` jump table.

| `cmd` | `len` | Effect |
| ----- | ----- | ------ |
| `0x0F` | 6 | Emit a uint32_t counter report. `cmd_0f_report_u32(G_COUNTER)` stages `(RX[5] & 0x7F) << 1` as a sub-id byte and `G_COUNTER` as a big-endian 4-byte value into RAM `0x20000141..0x20000145`, then `emit_counter_status_pdu` (OEM @ 0x08003C1C) lays out a **9-byte response PDU**: `[slave, len, sub-id, val[31..24], val[23..16], val[15..8], val[7..0], crc_lo, crc_hi]`. Note: the dispatcher's `len == 6` post-hook also fires `modbus_reply_passthrough()`, so the bus sees a 9-byte data PDU immediately followed by an 8-byte echo. The sub-id byte aliases `G_VERSION_BYTE` and the value clobbers `G_PKT_BYTES` — same RAM as the image-status emit slots. |
| `0x14` | 6 | When the motor is idle (`G_MOTOR_RUNNING == 0`): `G_COUNTER++`, set `G_14_FLAG_A = 1`, `G_14_FLAG_B = 1`. Otherwise: clear `G_14_FLAG_B`. The motor-running gate prevents the bike from incrementing the counter mid-shift. |
| `0x5A` | 6 | When the motor is idle (`G_MOTOR_RUNNING == 0`): copy `G_RX_BUF[5]` into `G_5A_TARGET`. Encodes the shift direction (0 = forward, 1 = reverse) consumed by the per-iteration motor servoing step (`motor_drive_step`) in `main`. Once the motor reaches position (or the stall timeout fires), `G_5A_TARGET` self-latches to 2 ("arrived"). |
| `0x5B` | 6 | `cmd_5b_selftest` (OEM @ 0x08003BC4) — sample the two GPIOA inputs PA0 and PA1 (the same inputs the motor-position state machine in `main` watches) and emit a 4-state status code encoding the pair: `{PA0=0,PA1=0}=0`, `{PA0=1,PA1=0}=0x32`, `{PA0=0,PA1=1}=0x64`, `{PA0=1,PA1=1}=0x96`. The code is reported as a 7-byte image-status PDU via `emit_image_status_payload(0, code)` (OEM @ 0x08003B9E — a `cmd_5c_write3` variant that derives the sub-id byte from `(RX[5]&0x7F)<<1` instead of taking it as a parameter). So on the bus this looks like a probe-and-respond: send cmd 0x5B → receive a 7-byte status PDU whose payload byte = encoded `{PA0,PA1}`. |
| `0x5C` | 3 | `cmd_5c_write3(G_5C_REGS[0..2])` (`image.c`, OEM @ 0x08003B86) — splice the three previously-staged register bytes into the image-status emit slots (`G_VERSION_BYTE`, `G_PKT_BYTES[0..1]`) and fire `report_image_status`. The bus sees a 7-byte status PDU; if `G_IMG_OK` is set, this also triggers a SYSRESETREQ — but normally the short-form is used as a status ping rather than a reset trigger. |
| `0x5C` | 0x0F | Copy `G_RX_BUF[2]`, `[4]`, `[5]` into `G_5C_REGS[0..2]` and call `flash_settings_commit` (`flash_store.c`, OEM @ 0x080031E6). That helper persists `G_STATE_FC` + `G_COUNTER` (big-endian) + `G_5C_REGS[0..2]` into the **settings flash page at `0x08007800`** as 8 halfwords (one data byte per halfword, high byte cleared), clears the deferred-commit latch `G_5C_BUSY`, and resets the per-task flag bytes via `state_flags_reset`. Note: each halfword write goes through `settings_set_halfword` which **erases the whole 1 KB page** — so a single commit does 8 sequential page erases. The same commit helper is also fired from main's idle-reset epilogue (`sched_idle_reset`, after `G_STATE_FC` lands at 0) and from main's deferred-commit watchdog (50 ticks after `G_5C_BUSY` is latched). |
| `0x81` | — | `image_apply()`. Validate the receive-slot at flash `0x08001800`; on success latch `G_IMG_OK_FLAG`, on failure erase the slot and reset receive-state RAM bytes. |
| `0x82` | 0x10 | `cmd_82_fw_page` → `FUN_080039E6` — accept a 16-byte OTA payload page (long-frame only). |
| `0x95` | — | `flash_erase_pages(0x08001800, 12)` + set `G_RX_FRAME_MODE = 1` so subsequent frames are accepted as 45-byte OTA payloads. |
| _any other_ | — | Silent ignore (GCC switch falls through to the common epilogue). |

After the case body, dispatch always:
- emits a 6-byte passthrough reply if `len == 6` or `len == 0x0F`;
- clears `G_REQ_PENDING`.

## Sequencing observations

- The fact that `modbus_tx_finalize` only triggers the
  SYSRESETREQ when `len == 7` AND `G_IMG_OK_FLAG == 1` ties the
  reset specifically to **`report_image_status` after a successful
  `image_apply`**. The bike's main module sees a 7-byte ACK and
  the shifter resets ~immediately afterward; shifterboot then
  picks up the freshly-staged image.
- The "switch to long-frame mode after erase" (`cmd 0x95` ⇒
  `G_RX_FRAME_MODE = 1`) is the OTA write protocol's first step.
  Subsequent `cmd 0x82` frames each carry one 16-byte page; the
  main module sends as many as the image needs, then issues
  `cmd 0x81` to validate and reset.
- `cmd 0x5C` is overloaded by length: short form (`len == 3`)
  reads back the previously-stashed 3-byte register block to the
  bus via `cmd_5c_write3` (`image.c`, see above); long form
  (`len == 0x0F`) writes 3 bytes into the same block from the
  inbound frame. The short form acts like a status ping that
  carries whatever was last written, packaged as the 7-byte
  image-status PDU.

## Open questions

- **Heartbeat / status pulse?** The main module presumably polls
  the shifter periodically; the round-robin in `main` looks like
  it stages outbound updates by tick index but the cases aren't
  fully decomp'd yet.
- **Long-frame payload format inside `G_LONG_BUF`** is unmapped —
  needs `FUN_080039E6` (the OTA-page consumer) decomp'd first.
- **`G_5A_TARGET`** is the cmd 0x5A shift-direction byte; its
  consumer is `motor_drive_step` → `motor_h_bridge_set`, both
  decomp'd. The exact PA9/PA10 bit mapping for each mask is now
  documented in `hardware.md`. The motor-running gate on cmd 0x14
  and cmd 0x5A means the bike can't change the shift target
  mid-move; it must wait for `G_5A_TARGET` to self-latch back to
  the "arrived" state.

## Sources

- `~/ghidra_scripts/DecompileOne.java <addr>` reproduces each entry.
- The literal pools at `0x080041FC..0x080045B7` carry the global
  addresses; resolve with `PeekBytes.java`.
- Cross-reference with `hardware.md` for the RAM-side semantics of
  every global named here.
