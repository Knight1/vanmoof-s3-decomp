# bleware — hardware notes

The BLE radio firmware on the VanMoof S3 sits on a **TI CC2642R1F** —
a Cortex-M4F + BLE 5.2 SoC. Same MCU as bleboot; this image is the
application that bleboot promotes into internal flash and jumps to.

| | |
| --- | --- |
| MCU | TI CC2642R1F (Cortex-M4F + BLE 5.2 SoC) |
| Core | ARM Cortex-M4F, Thumb-2, 48 MHz max (radio-side runs at 24 MHz) |
| Flash | `0x00000000..0x00057FFF` (352 KB total — last 8 KB belongs to bleboot) |
| SRAM | `0x20000000..0x20013FFF` (80 KB total) |
| External SPI flash | OAD staging slots — see bleboot's docs |
| Vector table | flash `0x00000000`, 50 entries (16 system + 34 IRQ) |

## Binary identity

Two image versions exist on the OEM tree:

### `bleware_1.4.01.bin`

| | |
| --- | --- |
| Size | 181884 bytes (0x2C67C) |
| OAD header | first 144 bytes (`hdrLen = 0x002C`, `prgEntry = 0x00000090`) |
| Image bytes | file offset 0x90..0x2C67B (181612 bytes) |
| SoftVer (per header) | `01.04.01` |
| Image CRC32 | `0xB79C4373` (stored at OAD header offset +8) |
| SHA-256 | `ee209726b535a5fcf3e6cdd2a3d1468b1e7cddeeab3dce99e76bd4edab0fde6d` |
| SHA-512 | `118084995f7423cf8b1c5589d49b20f203c06a4116213b4264a4c30d25060fee2fe057e1906e8a3ec9ab5323a02b2f72ee454fbc2c9cc7e6ca550ab71abcfe52` |

### `bleware_2.4.01.bin`

| | |
| --- | --- |
| Size | 217884 bytes (0x3531C) |
| OAD header | first 144 bytes |
| Image bytes | file offset 0x90..0x3531B (217612 bytes) |
| SoftVer | `02.04.01` |
| Image CRC32 | `0x884A9283` |
| SHA-256 | `89ad38f4213b375f59dd002c7e1174ac1de1086680262d91c6cbf14691ff20a2` |
| SHA-512 | `467f425f8ff329204876159697a71e04dec2b9fc7336892d233f68d7ce8ab8a4eb9b3dea506d5f885008a602301eb9a2ecbba66327379eb860115edd37a3057c` |

The hashes are the contract — if a future blob differs by a single byte, it's a different bleware.

## OAD-NVM1 image header (first 0x90 bytes)

The image ships with an 0x90-byte TI OAD-NVM1 header that bleboot uses
to validate + promote the image into internal flash. The header is
stripped at promotion time so the runtime image at flash `0x00000000`
starts directly with the vector table.

| Field | Offset | Width | bleware 1.4.01 | bleware 2.4.01 | Meaning |
| --- | --- | --- | --- | --- | --- |
| `imgID` | 0x00 | 8 B | `"OAD NVM1"` | `"OAD NVM1"` | NVM-bank-1 (external-flash) OAD image format |
| `crc32` | 0x08 | 4 B | `0xB79C4373` | `0x884A9283` | precomputed CRC32 over the image (excluding crcStat byte) |
| `bimVer` | 0x0C | 1 B | `0x03` | `0x03` | BIM expects ≥ this version |
| `metaVer` | 0x0D | 1 B | `0x01` | `0x01` | OAD metadata format version |
| `techType` | 0x0E | 2 B | `0xFFFE` | `0xFFFE` | tech-type word (BLE = 0x0001, but stored as little-endian `01 00` — `FE FF` here may indicate "any") |
| `imgCpStat` | 0x10 | 1 B | `0xFF` | `0xFF` | copy status — `0xFC` = "needs copy", `0xFE` = "promoted" |
| `crcStat` | 0x11 | 1 B | `0xFF` | `0xFF` | CRC validity — `0xFE` once BIM has verified |
| `imgType` | 0x12 | 1 B | `0x07` | `0x07` | image type — 0x07 = "application" |
| `imgNo` | 0x13 | 1 B | `0x00` | `0x00` | image slot number |
| `imgValidation` | 0x14 | 4 B | `0xFFFFFFFF` | `0xFFFFFFFF` | unused in OAD-NVM1 |
| `len` | 0x18 | 4 B | `0x0002C67C` | `0x0003531C` | total image length including header (matches file size) |
| `prgEntry` | 0x1C | 4 B | `0x00000090` | `0x00000090` | post-header offset to the program image |
| `softVer` | 0x20 | 4 B | `00 01 04 01` | `00 02 04 01` | software version (BCD-ish: `00 MM mm pp`) |
| `imgEndAddr` | 0x24 | 4 B | `0x0002C67B` | `0x0003531B` | absolute end-of-image address (= len - 1) |
| `hdrLen` | 0x28 | 2 B | `0x002C` | `0x002C` | core-header length |
| `rfu` | 0x2A | 2 B | `0xFFFF` | `0xFFFF` | reserved |
| (signature, future) | 0x2C..0x8F | 100 B | ... | ... | extended header — security/signature material |

## Memory map

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (bleware) | `0x00000000` | `0x00055FFF` | bleware runtime image (after bleboot promotion) |
| Flash (BIM)    | `0x00056000` | `0x00057FFF` | bleboot (TI BIM — see `bleboot/`) |
| CCFG           | `0x00057FA8` | `0x00057FFF` | Customer Configuration block (last 88 bytes of MCU flash; lives inside the BIM page) |
| SRAM           | `0x20000000` | `0x20013FFF` | 80 KB |
| Peripherals    | `0x40020000` | `0x4009FFFF` | CC2642 standard peripheral block |
| AON / AUX      | `0x40080000` | `0x400CFFFF` | always-on domain + AUX domain |

Initial SP from bleware 1.4.01's vector slot 0: `0x20013A00` (1.5 KB
below SRAM top — bleboot's spec was `0x20014000`, so bleware leaves
room for a small stack-overflow guard and/or BIM-shared RAM).

## Vector table (head)

Slot 1 = Reset_Handler at flash `0x0001F590` (file offset 0x1F620 once
the OAD header is stripped). Remaining vector slots TBD — to be
populated as decomp progresses.

## External SPI flash — secrets store

The external SPI flash chip (accessed via TI NVS + chip-driver wrappers)
hosts a 4 KB **secrets sector** at flash offset `0x0005A000`. The sector
is laid out as **128 records × 32 B**; each record is a payload + CRC-32:

```
record[0..27]   payload (28 B)
record[28..31]  CRC-32/zlib of payload  (little-endian, no final XOR)
```

`secrets_record_read(index, out)` (OEM `0x00020BB8`) reads slot
`index` (bounds-checked to `[0, 127]`), verifies the stored CRC against
a fresh CRC over the payload, and on success copies all 32 B to `out`.

`secrets_record_write_verify(index, record)` (OEM `0x00020C06`)
writes the 32 B record via the sector read-modify-write helper, reads
back, and `memcmp`-verifies. Up to 4 retries on mismatch. **Does NOT
bounds-check `index`** — preserved verbatim because callers may rely
on the elided check; treat as an OEM quirk and audit before any
"fix".

### Known slot assignments

Cross-checked against VanMoof's `VanMooof-Module` Go tool
(`ReadSecrets()`):

| Slot | Sector offset | Flash address | Length | Field |
| --- | --- | --- | --- | --- |
| 0      | `0x000` | `0x005A000` | 16 B | BLE Authentication Key (first 16 B of payload) |
| 0..123 | varies | varies | 32 B | User-keyed records: 32-bit key at `payload[+16]`, 4-byte ASCII tag at `payload[+24]` (e.g. `"UKEY"`). Lookups linear-scan by key; free slots = CRC-invalid slots. |
| 124    | `0xF80` | `0x005AF80` | 32 B | M-ID directory record — CRC-protected, tag `"M-ID"` at `payload[+24]`. Initialised by `secrets_ensure_mid_record` if missing. `payload[+16]` is a uint32 of unknown meaning (counter / cursor). |
| 125    | `0xFA0` | `0x005AFA0` | 32 B | likely M-ID/M-KEY continuation — TBD; no decoded firmware path touches it yet. |
| 126    | `0xFC0` | `0x005AFC0` | 16 B | Manufacturing Key (first 16 B of payload) |

The `VanMooof-Module` Go tool's `ReadSecrets()` slams a 60-byte raw
read at `0x005AF80` spanning slots 124+125 and calls the result
"M-ID/M-KEY". The firmware's own view is different: slot 124 is a
CRC-protected directory record with the `"M-ID"` tag, and slot 125's
framing is still TBD (likely also CRC-protected per record). The Go
tool reads the raw flash bytes and doesn't enforce the per-record CRC
that the firmware writes.

### Keyed-record API (slots 0..123)

A second API layer in the firmware treats slots 0..123 as a content-
addressed table:

| OEM address | Function | Role |
| --- | --- | --- |
| `0x00022BAA` | `secrets_find_by_key(key, out)` | scan for a record with `payload[+16] == key` |
| `0x00026034` | `secrets_count_free_slots()` | count slots with invalid CRC (= free) |
| `0x0001CA68` | `secrets_upsert_keyed_record(rec24)` | upsert (overwrite on key match, else first free slot); stamps tag `"UKEY"` |
| `0x0001F0BE` | `secrets_upsert_keyed_batch(arr, n)` | upsert N records; pre-checks total room |
| `0x00021328` | `secrets_ensure_mid_record()` | initialise slot 124 if its CRC fails; returns `payload[+16]` on existing record, else 0 |

### Implementation note — searching for the base address

The base address `0x0005A000` is encoded in the firmware as a Thumb-2
*modified-immediate* `add.w` instruction (e.g. `add.w r0, r0, #0x5a000`
at `0x00020BD2`), not as a 32-bit literal pool entry. A naive
little-endian byte search for `00 A0 05 00` will not find this constant
anywhere in flash — searches for SPI-flash region addresses need to
walk MOVW/MOVT pairs and `add.w` immediates.

## External SPI flash — FMNA provisioning (FMI build only)

The FMI image (`bleware_2.4.01.bin`) keeps Apple's Find My Network Accessory
provisioning data in a 16 KB external-flash region at `0x7B000..0x7EFFF`
(`0x7C000` = live record + `0x7B000` swap). The live record is a 1328-byte
AES-128-CBC blob (key derived from the FCFG1 BLE MAC) followed by its SHA-256 at
`0x7C7E0`; decrypted, it holds the accessory serial, the 1024-byte software-auth
token, its UUID, and the Apple server public keys (P-224 Q_A, P-256). This
region is **absent from the 1.4.01 target**. Full layout, key derivation, and
the function map: see `docs/fmna_storage.md`.

## Initial state (TBD)

Several pieces still to figure out:
- Which SimpleLink SDK version was bleware 1.4.01 / 2.4.01 built against? bleboot 1.0.0 matched `jeandudey/cc13x2_26x2_sdk` 3.40 (Apr 2020). bleware 1.4.01's image date is also `Apr 23 2020` per the OAD header convention, so 3.40 is the likely candidate.
- Memory layout of the SimpleLink ROM patches — the CC2642R1F has a sizeable ROM at `0x10000000..0x1003FFFF` containing BLE-stack ROM patches that any application image leans on.
- CCFG location and contents (lives in the BIM page, separate decomp territory under `bleboot/`).
