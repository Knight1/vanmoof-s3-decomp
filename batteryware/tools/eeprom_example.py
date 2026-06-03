#!/usr/bin/env python3
"""Generate an EXAMPLE VanMoof S3 batteryware data-EEPROM image (6 KB).

Produces a known-good 0x1800-byte EEPROM you can flash to 0x08080000 to
recover a pack with a corrupt/wrong EEPROM. Field offsets come from the
1.17.1 decomp (see ../docs/eeprom.md) — confirm against your firmware.

The firmware self-heals a lot: an ALL-ZERO EEPROM already boots safely and
factory-inits (FW-magic mismatch rewrites defaults; provisioned-flag 0 runs
factory init). This generator writes explicit defaults on top of that so the
image is human-readable and the version-magic matches.

What this CANNOT recover (pack-specific, must be redone on the pack):
  * ESN + manufacture date  -> write with the PBU "WriteESNAndDate" (Modbus
    func 0x10, regs 0x0C-0x14) after flashing.
  * CHG / DSG current calibration -> re-run the "CHG CAL" / "DSG CAL" console
    commands; the gauge is inaccurate until then.
  * Learned SOC / capacity / cycle count -> reset; the gauge re-learns.

Usage:
    eeprom_example.py [--version 1.14.1|1.17.1] [--capacity MAH]
                      [--esn ESN14] [--date YYYYMMDD] [-o out.bin]
"""
import sys
import struct

EE_SIZE = 0x1800
MAGIC = {"1.14.1": 0x011401B1, "1.17.1": 0x011701B1}


def put(buf, off, data):
    buf[off:off + len(data)] = data


def put_u16(buf, off, v):
    put(buf, off, struct.pack("<H", v & 0xFFFF))


def put_u32(buf, off, v):
    put(buf, off, struct.pack("<I", v & 0xFFFFFFFF))


def encode_esn(buf, esn):
    """14 ASCII chars -> regs 0x0C-0x12, byte-swapped per register cell."""
    esn = (esn.encode("ascii") + b"\x00" * 14)[:14]
    for i in range(7):
        base = 0x0F + 2 * i
        buf[base] = esn[2 * i + 1]   # low EEPROM addr = 2nd char of pair
        buf[base + 1] = esn[2 * i]   # high addr       = 1st char of pair


def encode_date(buf, yyyymmdd):
    y = int(yyyymmdd[0:4]) % 100
    m = int(yyyymmdd[4:6])
    d = int(yyyymmdd[6:8])
    buf[0x1D] = y
    buf[0x1E] = 0x00
    buf[0x1F] = d
    buf[0x20] = m


def main():
    a = sys.argv
    version = a[a.index("--version") + 1] if "--version" in a else "1.14.1"
    capacity = int(a[a.index("--capacity") + 1]) if "--capacity" in a else 12600
    esn = a[a.index("--esn") + 1] if "--esn" in a else None
    date = a[a.index("--date") + 1] if "--date" in a else None
    out = a[a.index("-o") + 1] if "-o" in a else "eeprom_example.bin"
    if version not in MAGIC:
        print("unknown version %r (use 1.14.1 or 1.17.1)" % version)
        sys.exit(2)

    ee = bytearray(EE_SIZE)  # erased EEPROM = 0x00

    # ---- Low area ----
    ee[0x01] = 0x00                       # boot mode: normal/safe (NEVER 0x17/0x18)
    put_u16(ee, 0x06, 0x0310)             # validated param defaults
    put_u32(ee, 0x2E, MAGIC[version])     # FW magic -> matches firmware
    put_u16(ee, 0x32, 0x0D01)
    ee[0x34] = 0x07
    put_u16(ee, 0x36, 0x0C4E)
    # anti-replay triplet left 0 -> next ESN/date write is accepted.

    if esn:
        encode_esn(ee, esn)
    if date:
        encode_date(ee, date)

    # ---- Config block @ 0x0C00 ----
    cb = 0xC00
    put_u32(ee, cb + 0x24, 0)             # stored SOC (re-learned)
    put_u32(ee, cb + 0x28, capacity)      # full-charge capacity (mAh); non-0 = provisioned
    put_u32(ee, cb + 0x2C, 0)             # remaining capacity
    put_u16(ee, cb + 0x34, 0)             # cycle count
    ee[cb + 0x36] = 0                     # RSOC %
    ee[cb + 0x37] = 100                   # absolute SOC full-ref
    put_u16(ee, cb + 0x3A, 1000)          # voltage threshold hi
    put_u16(ee, cb + 0x3C, 1000)          # voltage threshold lo
    ee[cb + 0x44] = ord("A")              # phase markers
    ee[cb + 0x45] = ord("A")
    ee[cb + 0x46] = ord("A")
    # secondary config @ 0x0C80 left all-zero (factory-init state).

    open(out, "wb").write(ee)
    print("wrote %s  (0x%X bytes, firmware %s, capacity %d mAh)"
          % (out, EE_SIZE, version, capacity))
    print("ESN : %s" % (esn or "(left blank — set via PBU WriteESNAndDate)"))
    print("date: %s" % (date or "(left blank — set via PBU WriteESNAndDate)"))
    print("\nFlash to 0x08080000, then on the pack:")
    print("  1. PBU WriteESNAndDate  (sets serial + manufacture date)")
    print("  2. CHG CAL / DSG CAL    (re-calibrate charge/discharge current)")


if __name__ == "__main__":
    main()
