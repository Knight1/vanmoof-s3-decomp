# bleware — Find My (FMNA) provisioning storage

> **Scope: the FMI build only.** This chapter documents `bleware_2.4.01.bin`,
> *not* the 1.4.01 decomp target. The Find My Network Accessory stack is absent
> from 1.4.01 (and from `bleware_1.4.01.bin`'s flash entirely) — it ships only in
> the FMI-capable images. It is documented here because the on-flash artefact
> (the blob at external-flash `0x7C000`) shows up in S3 SPI dumps and the
> `VanMooof-Module` Go tool now decodes it.
>
> Addresses below are **OEM (post-promotion flash) addresses**; the raw-file
> offset into `bleware_2.4.01.bin` is `OEM + 0x90` (the OAD header), matching the
> `flash 0x… (file offset 0x…)` convention in `hardware.md`.

## Apple FMNA-R5 SDK

2.4.01 embeds Apple's Find My Network Accessory reference stack. The upstream
source file names survive in assert/log strings:

```
source/xs3_fmna_image_provisioning.c   ← VanMoof platform glue
fmna-r5/platform/fmna_nvm_platform.c   ← NV storage platform layer
fmna-r5/platform/fmna_adv_platform.c
fmna-r5/fmna_crypto.c
fmna-r5/fmna_state_machine.c
fmna-r5/fmna_connection.c
fmna-r5/fmna_{paired_owner,nonowner,config}_control_point.c
```

`fmna_nvm_platform.c` + `xs3_fmna_image_provisioning.c` are the read/write/sync
path for the provisioning data. The firmware calls the provisioned record the
**"FMNA factory blob."**

## External-flash region

The FMNA NV region is **16 KB at external-flash `0x7B000..0x7EFFF`** (four 4 KB
sectors), erased as a unit by the `fmna-erase-external` monitor command. Layout:

| Sector | Role |
| --- | --- |
| `0x7B000` | swap sector (compaction scratch) |
| `0x7C000` | **main data (area A)** — the live v2 record |
| `0x7D000` | legacy v1 field area / spill |
| `0x7E000` | legacy v1 field area / spill |

It is a two-area, atomically-rewritten store: a write stages into `0x7B000`,
then the loader picks whichever of `0x7B000`/`0x7C000` is valid. A second
**internal** master copy lives in CC2642 on-chip flash; the platform re-syncs
the external area from it on boot (`"…not provisioned yet, checking internal"`,
`sync_image_provisioning_area`), so a wipe of `0x7C000` self-heals on reboot.

## v2 record format (area A, `0x7C000`)

| Offset | Size | Contents |
| --- | --- | --- |
| `0x7C000` | `0x530` (1328) | AES-128-CBC ciphertext (IV = 0), 83 blocks |
| `0x7C530` | `0x2B0` | `0xFF` erase padding |
| `0x7C7E0` | `0x20` | SHA-256 of the first `0x7E0` B (`ciphertext ‖ padding`) |

The SHA-256 covers the *ciphertext*, so it validates the read independently of
the key. Verified against an OEM dump: `SHA-256(ct ‖ 0xFF…) == bytes@0x7C7E0`.

### Encryption — device-bound

The AES-128 key derives from the CC2642 factory BLE MAC at `FCFG1 + 0x2E8`
(`0x500012E8`):

```c
/* key_derive @ 0x2A538 */
for (i = 0; i < 16; i++)
    key[i] = mac[i % 6] + i;     /* mac = FCFG1 little-endian order */
```

`mac` is the six MAC bytes as stored in FCFG1 — the reverse of the printed
`AA:BB:CC:DD:EE:FF`. The key is therefore per-device: a blob cloned to another
bike will not decrypt. (Binding, not strong secrecy — the MAC is semi-public.)

### Decrypted record (0x530 B)

Field offsets recovered from the v1 migration loader (`0xD944`) and confirmed by
decrypting an OEM dump:

| Offset | Size | Field |
| --- | --- | --- |
| `+0x000` | 1 | format version (`0x02`) |
| `+0x001` | 16 | accessory serial number (ASCII, NUL-padded — e.g. `"ASY3113989"`) |
| `+0x011` | 8 | metadata / flags |
| `+0x019` | 65 | EC P-256 public key (`0x04 ‖ X ‖ Y`) |
| `+0x05A` | 65 | EC P-256 public key (`0x04 ‖ X ‖ Y`) |
| `+0x09B` | 16 | software-authentication UUID |
| `+0x0AB` | 1024 | **software-authentication token** (Apple, ASN.1/DER) |
| `+0x4AB` | 32 | key material |
| `+0x4CB` | 57 | EC P-224 public key — Apple server key Q_A (`0x04 ‖ X ‖ Y`) |
| `+0x504` | 32 | key material |
| `+0x524` | 12 | unused (`0xFF`) |

The token decodes as DER (`openssl asn1parse`): a SET of SEQUENCEs carrying an
embedded ECDSA signature. The in-RAM struct base is `0x200048F0`; the decrypted
record loads at struct `+1` (version byte at `+0`).

### Legacy v1 layout

Before v2 packed everything into the single encrypted `0x7C000` blob, the fields
were stored per-sector (`fmna_nvm_platform.c`, plaintext + per-area integrity):

| Sector | Read | Fields |
| --- | --- | --- |
| `0x7E000` | `0x1C` | serial + metadata (24 B) |
| `0x7D000` | `0x86` | two 65 B P-256 public keys |
| `0x7C000` | active area | 16 B field + 1024 B token |
| `0x7F000` | `0xD9` | 32 B + 57 B P-224 (Q_A) + 32 B, HMAC-verified |

`0xD944` migrates this v1 layout into a v2 record (`"Attempting migration"`,
`"v1 swap not cleared"`, `"Incompatible storage downgrade"`).

## Function map (`bleware_2.4.01.bin`)

| OEM addr | file off | Role |
| --- | --- | --- |
| `0x14F08` | `0x14F98` | NVM init: validate/select active area, call loader, handle migration |
| `0x248C4` | `0x24954` | **record loader**: read `0x530`@area + `0x20`@area+`0x7E0`, SHA-256 verify, AES-CBC decrypt, require version == 2 |
| `0x25380` | `0x25410` | AES-128-CBC decrypt in place (83 × 16 B, IV = 0) |
| `0x2A538` | `0x2A5C8` | derive AES key from FCFG1 MAC |
| `0x1BB84` | `0x1BC14` | SHA-256 over `ciphertext ‖ 0xFF` up to `0x7E0` |
| `0x215B0` | `0x21640` | decrypting external-flash read primitive (mode 4/5 select) |
| `0x18308` | `0x18398` | area "has data?" probe (4 KB-chunk all-`0xFF` test) |
| `0x1DDE8` | `0x1DE78` | swap → main commit / compaction (erase + copy + header) |
| `0xD944`  | `0xD9D4`  | v1 → v2 migration loader (field map source) |
| `0x29F84` | `0x2A014` | `fmna-erase-external` (erase 0x7B000/0x7C000/0x7D000/0x7E000) |

These functions are part of the Apple FMNA-R5 platform port; when the 2.4.01
image is picked up as a decomp target they go in a `src/fmna_nvm_platform.c` /
`src/xs3_fmna_image_provisioning.c` pair. The same external-flash SPI driver and
secrets-store primitives documented in `hardware.md` underlie them.

## Tooling

`VanMooof-Module -f dump.rom -fmna` decodes the blob from a flash dump
(serial, UUID, token, keys), deriving the key from the BLE MAC (auto-detected
from the dump's secrets sector or boot log, or via `-fmna-mac`). See that
project's `FMNA.md`.
