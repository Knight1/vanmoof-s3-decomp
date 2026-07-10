# ⚠ BMS bootloader OTA — new in 1.09.01

mainware 1.09.01 adds a path to update the **battery module's bootloader** (BMS-BL),
separate from the batteryware-*application* update that 1.08.02 already had. The
main controller now **carries a BMS-BL image in its own flash** and pushes it to the
battery MCU over Modbus.

**New strings (all absent from 1.08.02):** `Start battery bootloader update`,
`BMS-BL CRC error`, `Wait for BMS-BL internal update!`, ` Please update BMS
bootloader first`, `No need for BMS-BL update`, `Wrong BMS-BL version`, `Wrong BMS
file`, and the menu label `BatteryBL update`. 1.08.02 had only `Start batteryware
update` / `Wait for BMS internal update!`.

## File validation & targeting — `FUN_0802B1E4`

A switch state machine that classifies an incoming update file:

- **case 4 — battery *application* file:** header CRC via `FUN_0802D9A0` (copies the
  0x28-byte / 10-word header, recomputes CRC over the payload with the hardware CRC
  engine `FUN_080214E2`, compares the stored CRC word; payload size capped
  `< 0x40000`). Mismatch → `BMS CRC error` (`0x08041751`). The header device byte
  must be **`0xB1`** (battery), else `Wrong BMS file` (`0x08041787`) / `Not a BMS
  file`. **New guard:** if the connected device's file-type field `*(ctx+0x45A)==6`
  and status low-byte `*(ctx+0x418)==0x14`, it prints ` Please update BMS bootloader
  first` (`0x08041761`) — the device is in a state that requires the BL be updated
  before an app can be flashed. On accept it logs `Batteryware: %X.%02X.%02X` and
  sets the download source = `*(hdr+0xC)`.
- **case 5 — BMS-BL file (NEW):** validates the BL image via `FUN_0802DA18` (CRC over
  **`0x1400` bytes**); fail → `BMS-BL CRC error` (`0x080417E0`). Reads the BL version
  from the in-flash BL blob at **`0x080E4F00`** (bytes `+0xF9/+0xFA/+0xFB` = 3
  version bytes), formats via `FUN_0803F9B0`, logs `BMS-BL: %03X (%s %s)`
  (`0x080417CB`). Sets the download **target start = `0x5000`** (the battery MCU's BL
  update entry) and **source base = `0x080E0000`** (the BMS-BL image embedded in
  mainware), then arms the transfer with `FUN_0803C5DC(0x14, 2, …)`.

So mainware itself **ships the battery bootloader binary** (≈`0x1400` bytes at flash
`0x080E0000`, version at `+0xF9..+0xFB`).

## Update decision — `FUN_0802DB58` (OTA driver, case 0x16 sub-case 6)

Polls the connected battery (status word `*(ctx+0x45A)` past a ready threshold
~25000, `*(ctx+0x418)` valid), reloads the embedded BL blob version
(`0x080E0000`-region `+0xF9/FA/FB` via `FUN_0803F9B0`), and compares to the device's
reported BL version:

```
if (embedded_BL_ver == device_BL_ver)        -> "No need for BMS-BL update"   (0x080420DB), done
else if (high byte matches)  /* compatible/upgradeable */
                                             -> FUN_0802C328 (state=5), proceed to push
else                                         -> "Wrong BMS-BL version"         (0x080420F7), abort
```

(Sub-case 5 does the analogous compare for the batteryware *application* version.)

## Push — Modbus RTU to the battery (slave 0xAA)

The transfer uses `FUN_080326F4` → `FUN_08033D54` (Modbus master TX), packing
little-endian frames in a stack header. The battery is driven into its bootloader
and the embedded BL image at `0x080E0000` is streamed to BL entry `0x5000`, with
`Wait for BMS-BL internal update!` printed while the battery applies it internally.

## Why it matters

This is a **new, sensitive OTA surface**: the main controller can reflash the
*bootloader* of the battery MCU (not just its application). A bad BL image or an
interrupted BL flash is far less recoverable than an app update — hence the explicit
CRC (`0x1400`-byte), the version-compatibility gate (high-byte match only), and the
`Please update BMS bootloader first` ordering guard. None of this exists in 1.08.02.
