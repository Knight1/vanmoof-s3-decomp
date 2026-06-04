# bmsboot — image format & download protocol

## Application image header (40 bytes, prefixes AP / Shadow)

| Offset | Size | Field |
| --- | --- | --- |
| `+0x00` | 4 | magic `0xAA55AA55` |
| `+0x04` | 4 | packed version |
| `+0x08` | 4 | CRC-32 (over header with `+0x08`/`+0x0C` masked, then body) |
| `+0x0C` | 4 | image size in bytes (must be `< 0x15801`) |
| `+0x10` | 0x18 | build date/time + padding |
| `+0x28` | … | real Cortex-M0+ vector table (SP `+0x28`, reset `+0x2C`) |

`image_verify()` (returns `0` OK / `1` CRC-bad / `2` magic-or-size-bad):

1. `magic == 0xAA55AA55` and `size < 0x15801`, else `2`.
2. copy the 40-byte header to scratch, set the crc (`+0x08`) and size (`+0x0C`)
   words to `0xFFFFFFFF`.
3. reset the HW CRC unit (`CRC->CR.RESET`), accumulate the 10 masked header words,
   then the body words `[+0x28 .. +size)`.
4. compare against the stored crc (`+0x08`); equal → `0`, else `1`.

This is byte-compatible with the build-time patcher the application tree uses
(`../tools/patch_image_header.py`: it blanks `body[8:16]` before CRC-ing).

## Persisted boot flag (EEPROM `0x08080000`)

`main()` reads it once at startup:

| Value | Meaning | Action |
| --- | --- | --- |
| `0x55` | normal | run the super-loop (validate AP on the first tick → boot) |
| `0xCC` | recover | if Shadow is valid, install Shadow→AP and boot |
| `0x33` | acknowledge | rewrite to `0x55`, run the loop |
| `0x5A` | wipe | rewrite to `0x55`, erase AP **and** Shadow, run the loop (force download) |

A successful boot writes `0` to the flag (`goto_application` path). After an OTA
finalise the loop mirrors AP→Shadow and rewrites `0x55`.

## Serial-download ("WHO?") protocol — USART1 @ 9600 8N1

Only active when `download_pin_check()` saw PA10 high (or the host drives the bus).
The host streams one byte at a time; the loader replies a single byte
(`0x79` 'y' = ACK, `0x1F` = NAK). Every reply resets the frame machine to idle.

### Handshake / command header (state IDLE)

```
'W' 'H' 'O' '?' '\r'           -> loader transmits its banner ("I am VanMoof BL …")
<cmd> <~cmd>                    -> ACK, enter ARG state with cmd
```

`<cmd>` is `0x11`, `0x21` or `0x31`; the second byte is its bitwise complement
(`cmd ^ comp == 0xFF`, i.e. `0x11/0xEE`, `0x21/0xDE`, `0x31/0xCE`). A leading `\n`
(`0x0A`) resets the parser. `'W'/'H'/'O'` also accept the lowercase forms.

### Address frame (state ARG)

```
b0 b1 b2 b3 x                  x == b0^b1^b2^b3 ; value = (b0<<24|b1<<16|b2<<8|b3)
```

`value` must be `> 0x08004FFF` (i.e. in the AP bank). Then, by command:

- **`0x11` / `0x31`** — latch the write address, ACK, enter DATA state, set
  `g_loop_flags` bit0 (busy).
- **`0x21`** — finalise. If `value == 0x08005000` (AP base) program the held-back
  first page from scratch (erase+verify retry), ACK, flush TX, clear busy and set
  `g_loop_flags` bit1 (upgrade-finished); else NAK.

### Data block (state DATA)

```
L  d0 .. dL  x                 L = block length byte; L+1 payload bytes follow;
                               x == L ^ d0 ^ .. ^ dL   (running XOR, seeded with L)
```

A full block carries 128 bytes (`L = 0x7F`) — one STM32L0 flash page. On the
trailing XOR match:

- **target == AP base (`0x08005000`)** — the page that holds the image header is
  **not** programmed; it is stashed in `s_first_page` and the page is only erased.
  It is committed later by the `0x21` finalise command. This makes the install
  atomic: an interrupted download never leaves a valid magic at AP base, so the
  loader refuses to boot the partial image.
- **any other page** — erase + `flash_program_verify` (64-byte half-pages), retry
  until it programs clean.

Then ACK and kick the watchdog. The host advances the address with a fresh
`0x11`/`0x31` command before each block.

## Super-loop events (`SysTick_Handler` → `g_boot_events`)

SysTick increments a 0..999 sub-divider and posts widening event-bit sets at the
10/50/100/250/500-tick boundaries (`0x03`/`0x07`/`0x0F`/`0x17`/`0x3F`) and the full
set `0x7F` at the 1000-tick rollover. `main()` services them: **bit2** paces the
boot decision (validate AP → boot, else recover from Shadow, else wait and count
`g_boot_countdown` down); **bit6** kicks the IWDG while idle; every pass drains the
RX ring into `ota_process_byte()` and pumps the TX ring.
