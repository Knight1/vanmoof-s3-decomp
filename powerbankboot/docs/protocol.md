# powerbankboot — protocol

Two things live here: the **image format** the loader validates, and the
**serial-download protocol** it speaks when no valid image is present.

## Image header & CRC (`image_verify` @ `0x08001750`)

Each application bank starts with a 40-byte (`0x28`) header; the real vector
table follows at `+0x28`.

| Offset | Size | Field | Check |
| --- | --- | --- | --- |
| `+0x00` | 4 | magic | must be `0xAA55AA55` |
| `+0x04` | 4 | version | e.g. `0x011105B2` (1.11.05, type `0xB2`) |
| `+0x08` | 4 | crc32 | compared against the computed CRC |
| `+0x0C` | 4 | size | must be `< 0x1C001` (≤ 112 KB) |
| `+0x10` | 12 | build date | |
| `+0x1C` | 9 | build time | |
| `+0x25` | 3 | reserved | |
| `+0x28` | … | vector table + code | SP `@ +0x28`, reset `@ +0x2C` |

`image_verify()` returns **0 = OK**, **1 = CRC mismatch**, **2 = bad magic/size**
(and traces `"-->CRC Verify 0x%4x%4x = "` followed by the code as `"0\n\r"` /
`"1\n\r"` / `"2\n\r"`). The CRC is **MPEG-2 CRC32** (poly `0x04C11DB7`, init
`0xFFFFFFFF`, no reflection — the STM32 hardware CRC unit) over the whole image,
with the header words at **`+0x08` (crc32)** and **`+0x0C` (imageSize)** — i.e.
bytes `[0x08:0x10)` — blanked to `0xFFFFFFFF` first. The loader CRCs the 10 header
words then `(size - 0x28)/4` body words and compares to the stored `crc32`. This
is byte-identical to the build-time patcher `tools/patch_image_header.py`
(`patch_ware`: `body[8:16] = 0xFF`), which is verified against the OEM ware
images — so the `imageSize` field bounds the CRC length but is itself excluded
from the CRC content.

## Boot decision (`boot_main` @ `0x080014A0`)

```
boot_hw_init();                         clock/IWDG/RTC/UART/GPIO
boot_read_persistent_flags();           RTC BKP0R/BKP1R -> g_upgrade_finished

if (g_upgrade_finished == 1 && verify(Shadow) == OK):
        copy Shadow -> AP ;  goto_application()      # install a finished OTA
if (verify(AP) == OK):
        if (verify(Shadow) != OK): copy AP -> Shadow # keep a backup
        goto_application()                            # normal boot
if (verify(Shadow) == OK):
        copy Shadow -> AP ;  goto_application()       # recover a corrupt AP
# neither bank valid -> fall through to the download server loop
```

`goto_application()` (`0x080019C0`): trace `"-->Goto_Application()"`,
`store_boot_flag(0)`, refresh IWDG, load the app SP from `AP+0x28` and the reset
vector from `AP+0x2C`, set MSP, and branch. No `VTOR` on Cortex-M0 — the
application relocates its own vector table afterwards.

`flash_copy_image(dst, src)` (`0x08001824`) mirrors a 112 KB bank in 2 KB pages
(read → erase → program, retry on program error), then re-verifies the whole
destination and repeats until it passes. It traces `"Copy AP to Shadow"` or
`"Copy Shadow to AP"` from the source, and `"--> Done"` at the end.

## Serial-download server (`ota_process_byte` @ `0x08000220`)

When both banks are invalid the loader prints the banner **`"\nI am VM-BATT BL\r"`**
and services USART2 byte-by-byte. Replies are one byte: **`0x79` (`'y'`) = ACK**,
**`0x1F` = NAK**.

### Keepalive handshake

```
host: 'W' 'h' 'o' '?' '\r'   ->   loader: "I am VM-BATT BL"
```

The server loop also re-emits the banner / `"-->In BL"` periodically (a SysTick
keepalive divider) so a host can detect the loader at any time.

### Command framing

A command is a byte followed by its bitwise complement, then a 5-byte argument
(32-bit **big-endian** value + 1-byte XOR check, `x == b0^b1^b2^b3`):

```
<cmd> <~cmd>  b0 b1 b2 b3 x
```

| cmd | byte | ~cmd | meaning |
| --- | --- | --- | --- |
| erase / prepare | `0x31` `'1'` | `0xCE` | latch write address `value`, set busy, erase its page if `value & 0x7FF == 0`, enter the data phase |
| verify / finalise | `0x21` `'!'` | `0xDE` | if `value` equals the expected end address: ACK, flush TX, clear busy, set the *upgrade-finished* mode bit |

The argument `value` must satisfy `0x08007FFF < value <= 0x08023FFF` (inside the
AP bank); otherwise the loader NAKs.

### Data phase

After a `0x31` command, data arrives as length-prefixed, XOR-checked blocks:

```
L  d0 d1 … d(L-1)  x          x == (L+1) ^ d0 ^ d1 ^ … ^ d(L-1)
```

Each block is assembled into a 2 KB page buffer at offset `addr & 0x7FF` and
programmed at the 256-byte-aligned address. On a program error the loader erases
the whole 2 KB page (`addr & ~0x7FF`) and re-programs it from the page buffer.
The IWDG is kicked around every flash operation. A good block is ACKed; a bad XOR
is NAKed.

### Finalising an update

The `0x21` finalise sets `g_loop_mode` bit 1. The server loop (driven by a
SysTick event) notices it, re-verifies the freshly written **AP** and mirrors it
into **Shadow** (`"Copy AP to Shadow"`), then re-announces the loader. A
subsequent reset re-enters `boot_main`, finds AP valid, and boots it — so the
full field-update cycle is:

```
0x31 erase → data blocks → 0x21 finalise → (AP mirrored to Shadow) → reset → boot AP
```

### SysTick event bits (`SysTick_Handler` @ `0x0800214C`)

The tick posts bits into `g_boot_events`, consumed by the server loop:

| bit | cadence | server-loop action |
| --- | --- | --- |
| 0 | every tick | if finalise pending → mirror AP→Shadow, re-banner |
| 1 | ~every 50 ticks | keepalive divider (re-announce every ~100) |
| 2 | ~every 250 ticks | refresh the IWDG (unless a transfer holds the busy lock) |
| 3 | sub-divider rollover | reserved |

Event posting is gated on an "armed" validity pair set up by
`comms_rx_state_init` (`g_comms_a ^ g_comms_d == 0xFFFFFFFF`).
