# motorware container & boot-stream format

`motorware` is the VanMoof S3 motor-controller firmware for the Texas
Instruments **TMS320F28054F** (C2000 Piccolo, C28x DSP core — *not* ARM).
Two OEM images are on hand:

| File | Version | Built | content | file |
| --- | --- | --- | --- | --- |
| `motorware_S.0.00.22.bin` | `S.0.00.22` (current, in the `.pak`) | Mar 2021 | 61 720 B | 61 720 B |
| `motorware_S.0.00.15.bin` | `S.0.00.15` (older) | Apr 2020 | 61 204 B | 131 072 B |

`.15` is the same content padded with `0xFF` up to the full 128 KB flash; `.22`
is unpadded. Everything below is **verified byte-exact** by
`tools/bootstream.py`, which round-trips either file back to the original bytes
and recomputes the stored CRC. Run it with:

```
python3 motorware/tools/bootstream.py <image.bin>
python3 motorware/tools/bootstream.py --extract=motorware/build/image <image.bin>
```

## 1. Outer wrapper — VanMoof application-ware header (`0x00`–`0x27`)

Identical to the other S3 application wares (batteryware/mainware/shifterware),
so `tools/patch_image_header.py` handles it unchanged. 40 bytes:

| Offset | Size | `.22` value | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | `0xAA55AA55` | magic |
| `+0x04` | 4 | `0x000016A1` | version, little-endian bytes `TYPE,PATCH,MINOR,MAJOR` = `A1,16,00,00` → **type `0xA1`** (motor module), patch `0x16`=22, minor 0, major 0 → "S.0.00.22" |
| `+0x08` | 4 | `0x9E5DD658` | CRC32 |
| `+0x0C` | 4 | `0x0000F118` | `imageSize` = 61 720 (content length in bytes) |
| `+0x10` | 12 | `"…03 2021"` | build date (ASCII, NUL-padded) |
| `+0x1C` | 12 | `"00:48:35"` | build time |

* **Module type `0xA1`** sits alongside the other known type bytes
  (`0xAA` batteryware, `0xB1` … , `0xB2` powerbankware). Confirmed identical in
  both images (only the patch byte changes: `0x16`=22 vs `0x0F`=15).
* **CRC32** is the MPEG-2 variant (poly `0x04C11DB7`, init `0xFFFFFFFF`, no
  reflection, over little-endian 32-bit words) computed over the first
  `imageSize` bytes with header `[8:16)` blanked to `0xFF` — the same algorithm
  the STM32/MM32 wares use. Recomputed value matches the stored CRC exactly for
  both images.
* The `.22` build-date field reads as raw bytes `6D 08 20 30 33 20 32 30 32 31`
  (`"m\x08 03 2021"`); `.15` is a clean `"apr 09 2020"`. The stray `0x08` in the
  `.22` stamp is an OEM build-script quirk, not a parse error — it is preserved
  on round-trip.

## 2. Payload — TI C28x boot-ROM data stream (`0x28`–end)

From `+0x28` the body is a standard **C2000 bootloader data stream**
(SPRU430, *Bootloader Data Stream Structure*) — the format a C28x ROM SCI /
SPI / parallel / CAN bootloader consumes to load memory. Here it is stored in
flash and replayed by the motor MCU's flash kernel. All words are 16-bit
little-endian; C28x addresses are **word** addresses.

```
word  0      key            0x08AA   (8-bit boot stream)
word  1..8   reserved       8 × 0x0000
word  9,10   entry point    upper,lower  ->  PC = (upper<<16)|lower
            ──── then a sequence of blocks ────
            sizeW          block length in words; 0x0000 ends the stream
            dstHi, dstLo   destination word address (upper, lower)
            data[sizeW]    sizeW payload words
            …
            0x0000         terminator
            <1 trailing word = 0x0000 (pad)>
```

### `.22` block map (entry = `0x3F4799`)

| Block (stream order) | dest addr | size (words) | end | region |
| --- | --- | --- | --- | --- |
| 0 | `0x3F4F8C` | 607 | `0x3F51EB` | `.cinit` (variable-init table; referenced by `_c_int00`) |
| 1 | `0x3EE000` | 27 737 | `0x3F4C59` | main `.text` + `.const` |
| 2 | `0x3F7FFE` | 2 | `0x3F8000` | top-of-flash words (CSM password tail region) |
| 3 | `0x3F7000` | 1 353 | `0x3F7549` | secondary code/const region |
| 4 | `0x3F4C5A` | 818 | `0x3F4F8C` | continues `.text`/`.const` |
| 5 | `0x3F51EB` | 292 | `0x3F530F` | continues after `.cinit` |

Blocks 1,4,0,5 are emitted out of address order but tile into one near-contiguous
run. The full programmed footprint, in **address** order:

| Flash region (word addr) | words | bytes | content |
| --- | --- | --- | --- |
| `0x3EE000` – `0x3F4C59` | 27 737 | 55 474 | main `.text`/`.const` (ends at `0x3F4C58`) |
| `0x3F4C5A` – `0x3F530F` | 1 717 | 3 434 | more `.text`/`.const` + `.cinit` |
| `0x3F7000` – `0x3F7549` | 1 353 | 2 706 | secondary code/const |
| `0x3F7FFE` – `0x3F7FFF` | 2 | 4 | CSM-password-tail words `0x007F 0x4C19` |

* A **single-word hole at `0x3F4C59`** separates block 1 from block 4. It is a
  linker even-word section-alignment gap (`0x3F4C59` is odd; the next section
  starts at the even `0x3F4C5A`) and stays erased (`0xFFFF`).
* The C28x **flash entry redirect at `0x3F7FF6`** and most of the 128-bit CSM
  password (`0x3F7FF8`–`0x3F7FFD`) are **not** programmed (read `0xFFFF`). Only
  the password's last two words are written. The reset-to-flash hand-off
  therefore does not go through the usual `0x3F7FF6` branch; the boot-stream
  `entry` field (`0x3F4799`) is the application start (`_c_int00`). The motor
  MCU's persistent boot/flash kernel — not part of this app image — is what
  parses the stream and transfers control. (Mechanism to be confirmed against
  the datasheet boot flow; see `memory-map.md`.)
* `_c_int00` at the entry loads `0x3F4F8C` (block 0) into an XAR register —
  that address is the `.cinit` table, the standard TI C-runtime startup
  signature.

### `.15` vs `.22`

Same structure; only sizes/addresses shifted (older build): entry `0x3F468F`,
main block `0x3EE000` × 27 479 words, secondary block `0x3F7000` × 1 393 words.
Both round-trip byte-exact.

## 3. Reconstructed image artifacts

`bootstream.py --extract=DIR` writes each contiguous region as a raw
little-endian image (2 bytes per C28x word, erased = `0xFFFF`) plus
`manifest.json` mapping file → load word-address. These are the inputs for the
disassembler (see `memory-map.md` for the import recipe). They are generated,
never committed (`*.bin` is git-ignored).
