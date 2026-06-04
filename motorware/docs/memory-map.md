# Memory map — motorware (TMS320F28054F, C2000/C28x)

The motor controller runs on a TI **TMS320F28054F** — a C2000 Piccolo
fixed-point DSP, **not** an ARM core. All addresses below are **22-bit word
addresses**: the C28x is word-addressed, the smallest addressable unit is a
16-bit word, and `sizeof(char) == sizeof(int) == 1` (= 16 bits). Little-endian.

Authoritative sources: **SPRS797F** (datasheet), **SPRUHE5** (TRM),
**SPRU430F** (CPU/ISA), **SPRAC71C** (EABI/ABI), **SPRUGO0A** (Piccolo boot
ROM), and TI's `F28054.cmd` / `F2805x_Headers_nonBIOS.cmd` (C2000Ware). Every
address in the device tables below is from those primary sources.

## Device memory (F28054F)

| Region | Word range | Size | Notes |
| --- | --- | --- | --- |
| M0 SARAM | `0x000000`–`0x0003FF` | 1K | first `0x50` = boot-ROM stack; SP reset = `0x400` |
| M1 SARAM | `0x000400`–`0x0007FF` | 1K | |
| Peripheral frames 0–3 | `0x000800`–`0x007FFF` | — | data space only (CPU can't fetch here) |
| L0 DPSARAM | `0x008000`–`0x0087FF` | 2K | secure; doubles as CLA data RAM 2 |
| L1 DPSARAM | `0x008800`–`0x008BFF` | 1K | CLA data RAM 0 |
| L2 DPSARAM | `0x008C00`–`0x008FFF` | 1K | CLA data RAM 1 |
| L3 DPSARAM | `0x009000`–`0x009FFF` | 4K | **CLA program RAM — here used as the app's parameter/control RAM** |
| FLASH | `0x3E8000`–`0x3F7FFF` | 64K | 10 sectors **A–J** |
| User OTP | `0x3D7800`–`0x3D7FFF` | — | DCSM zone-select/passwords (Z2 `0x3D7800`, Z1 `0x3D7A00`), Device_cal |
| Boot ROM | `0x3FD000`–`0x3FFFFF` | 12K | IQmath tables (~`0x3FDB52`), boot code, CPU vectors |
| Reset vector | `0x3FFFC0` | 2 w | PC reset value; boot ROM → `InitBoot`/`SelectBootMode` |
| PIE vector table | `0x000D00`–`0x000DFF` | 256 w | enabled when `VMAP=1 & ENPIE=1` |

There is **no FPU and no VCU**; a CLA exists on the `F` variant but L3 (its
program RAM) is repurposed here as data, so the CLA is most likely unused. All
math is fixed-point **IQmath** (IQ24 global Q — confirmed in the image, see
`hardware.md`).

### Flash sectors (F28054, `F28054.cmd`)

| Sector | Origin | Length (w) |  | Sector | Origin | Length (w) |
| --- | --- | --- | --- | --- | --- | --- |
| FLASHJ | `0x3E8000` | `0x1000` |  | FLASHE | `0x3F0000` | `0x2000` |
| FLASHI | `0x3E9000` | `0x1000` |  | FLASHD | `0x3F2000` | `0x2000` |
| FLASHH | `0x3EA000` | `0x2000` |  | FLASHC | `0x3F4000` | `0x2000` |
| FLASHG | `0x3EC000` | `0x2000` |  | FLASHB | `0x3F6000` | `0x1000` |
| FLASHF | `0x3EE000` | `0x2000` |  | FLASHA | `0x3F7000` | `0x0FFE` |
| | | |  | **BEGIN** | `0x3F7FFE` | `0x0002` |

> **Boot-to-flash entry is `0x3F7FFE`** (the top 2 words, sector FLASHA) — *not*
> the legacy F2802x/F2803x `0x3F7FF6`. F2805x is a DCSM device: the old
> 128-bit CSM password block at `0x3F7FF8` does not exist; passwords live in
> User-OTP. This was verified against `F28054.cmd` and matches the image (see
> boot flow below).

## motorware flash layout (this image, `S.0.00.22`)

Reconstructed from the boot-stream blocks (see `container.md`; verified by a
byte-exact round-trip). Only the regions below are programmed; the rest of
flash reads `0xFFFF` (erased), including the lower sectors `0x3E8000`–`0x3EDFFF`.

| Flash region | Words | Content |
| --- | --- | --- |
| `0x3EE000`–`0x3F4C58` | 27 737 | `.text` + `.econst` (main application — all 356 discovered functions live here, incl. `_c_int00` `0x3F4799` and `wd_disable` `0x3F4C19`) |
| `0x3F4C59` | 1 | erased — linker even-word alignment hole |
| `0x3F4C5A`–`0x3F4F8B` | 818 | more `.text`/`.econst` |
| `0x3F4F8C`–`0x3F51EA` | 607 | **`.cinit`** — C auto-init table (97 records → L3 RAM globals; see `hardware.md`) |
| `0x3F51EB`–`0x3F530E` | 292 | tail `.econst` |
| `0x3F7000`–`0x3F7548` | 1 353 | secondary `.text` (function-pointer-reached IQmath/library code) |
| `0x3F7FFE`–`0x3F7FFF` | 2 | **codestart**: `LB 0x3F4C19` |

## Boot flow (verified)

```
reset → boot ROM @0x3FFFC0 → SelectBootMode → "boot to flash"
      → jump to BEGIN (0x3F7FFE)
      → LB 0x3F4C19            ; codestart, encoded 007F 4C19
      → wd_disable (0x3F4C19)  ; disable watchdog
      → LB _c_int00 (0x3F4799) ; xref'd from 0x3F4C1F
      → _c_int00: copy .cinit (0x3F4F8C) into L3 RAM, run .pinit, call main
```

The 2 words at `0x3F7FFE` decode as the C28x long branch `LB 0x3F4C19`
(`opcode 0x001`, addr[21:16]=`0x3F` in word0 `0x007F`, addr[15:0]=`0x4C19` in
word1). The boot-stream `entry` field (`0x3F4799`) is `_c_int00`. The on-chip
flash kernel that consumes the boot stream (and the DCSM/boot configuration)
is not part of this application image.

## Loading into a disassembler

Stock Ghidra 11.2 has **no** C28x processor module. This project uses **IDA**
(native `tms32028`). The reconstructed regions (`build/image/region_*.bin`,
2 bytes/word LE) load at their word addresses via `ida/build_db.py`; see
`ida/README.md`. The peripheral register frames are mapped as named segments.

### Peripheral register frame bases (data space)

| Base | Block |  | Base | Block |  | Base | Block |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `0x6000` | ECanaRegs |  | `0x6800`–`0x6980` | EPwm1–7Regs |  | `0x7050` | SciaRegs |
| `0x6400` | AnalogSubsys |  | `0x6A00` | ECap1Regs |  | `0x7100` | AdcRegs |
| `0x6F80` | GpioCtrlRegs |  | `0x6B00` | EQep1Regs |  | `0x7750` | ScibRegs |
| `0x7010` | SysCtrlRegs |  | `0x7040` | SpiaRegs |  | `0x7770` | ScicRegs |
| `0x000C00` | CpuTimer0 |  | `0x7900` | I2caRegs |  | `0x000D00` | PieVectTable |

Peripheral set (SPRS797F): **7× ePWM, 1× eCAP, 1× eQEP, 1× 12-bit ADC,
3× SCI, 1× SPI, 1× I2C, 1× eCAN, up to 7 comparators, 3× CPU timers, WD**.
