#!/usr/bin/env python3
"""Decode a VanMoof S3 batteryware data-EEPROM image.

The BMS keeps its persistent state in the STM32L072 on-chip data EEPROM at
0x08080000 (6 KB, 0x1800 bytes). This tool decodes the documented fields
(see ../docs/eeprom.md). Field OFFSETS are derived from the 1.17.1 decomp;
verify against your firmware version (the FW-magic field reports it).

Usage:
    eeprom_decode.py <eeprom.bin> [--base ADDR]

    <eeprom.bin>  a dump of the data-EEPROM region. If it is exactly 0x1800
                  bytes it is taken as 0x08080000..0x080817FF. A larger image
                  (e.g. a combined flash+eeprom read) needs --base, the file
                  offset that corresponds to address 0x08080000.

NOTE: a 192 KB (0x30000) file is *main flash only* and does NOT contain the
EEPROM — the EEPROM is a separate address region. Read 0x08080000 len 0x1800
to obtain it.
"""
import sys
import struct

EE_BASE = 0x08080000
EE_SIZE = 0x1800

# Known firmware versions by FW-magic value.
FW_MAGICS = {
    0x011401B1: "1.14.1",
    0x011701B1: "1.17.1",
}


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def decode_esn(ee):
    """ESN: 14 ASCII bytes in regs 0x0C-0x12 (EEPROM 0x0F-0x1C).

    Each Modbus register is stored as a little-endian cell, so the two ASCII
    chars of a register are byte-swapped in raw EEPROM. Read back as the
    firmware does: char order = E[odd], E[even] per pair.
    """
    chars = []
    for i in range(7):
        base = 0x0F + 2 * i
        chars.append(ee[base + 1])
        chars.append(ee[base])
    return bytes(chars)


def decode_date(ee):
    # write/read path: year@0x1D, 0x00@0x1E, day@0x1F, month@0x20
    year = ee[0x1D]
    month = ee[0x20]
    day = ee[0x1F]
    return year, month, day


def show_ascii(bs):
    return "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in bs)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    base = 0
    if "--base" in sys.argv:
        base = int(sys.argv[sys.argv.index("--base") + 1], 0)
    # --lead N: the dump is missing its first N bytes (file offset 0 is
    # address 0x08080000 + N). Left-pad so addresses realign.
    lead = 0
    if "--lead" in sys.argv:
        lead = int(sys.argv[sys.argv.index("--lead") + 1], 0)

    raw = open(path, "rb").read()
    if lead:
        raw = b"\x00" * lead + raw
        print("NOTE: --lead %d -> left-padded; treating file offset 0 as "
              "address 0x%08X.\n" % (lead, EE_BASE + lead))
    # Detect a main-flash image misfed as an EEPROM dump.
    looks_like_flash = (len(raw) >= 0x5004 and raw[0x5000:0x5004] == b"\x55\xaa\x55\xaa")
    if base == 0 and looks_like_flash:
        print("ERROR: this is a MAIN-FLASH image (app header 55 AA 55 AA at "
              "file 0x5000), not the data EEPROM.\n"
              "       The EEPROM is a separate region at 0x%08X (len 0x%X) and "
              "is NOT inside a flash dump.\n"
              "       Dump it with e.g.:  st-flash read eeprom.bin 0x%08X 0x%X"
              % (EE_BASE, EE_SIZE, EE_BASE, EE_SIZE))
        sys.exit(1)
    if base == 0 and len(raw) > EE_SIZE:
        print("WARN: file is 0x%X bytes (not 0x%X). Assuming EEPROM at file "
              "offset 0 — pass --base if it lives elsewhere.\n" % (len(raw), EE_SIZE))
    ee = raw[base:base + EE_SIZE]
    have_cfg = len(ee) >= 0xCC0
    if len(ee) < EE_SIZE:
        print("NOTE: only 0x%X bytes present (full EEPROM is 0x%X). The low "
              "area (boot/ESN/date/params) is here; the config block at "
              "0x08080C00 (SOC/cycle-count/capacity/thresholds) is %s.\n"
              % (len(ee), EE_SIZE, "present" if have_cfg else "NOT in this dump"))
        ee = ee + b"\x00" * (EE_SIZE - len(ee))

    print("=== batteryware data-EEPROM (base 0x%08X) ===\n" % EE_BASE)

    # ---- Low area: boot / identity / params ----
    boot = u8(ee, 0x01)
    boot_note = {0x17: "MOS-failure (DANGER: arms fuse path)",
                 0x18: "MOS-failure", 7: "OVP1 re-entry", 8: "OVP2 re-entry",
                 9: "UVP1 re-entry", 10: "UVP2 re-entry"}.get(boot, "normal")
    print("boot mode      @0x01 : 0x%02X  (%s)" % (boot, boot_note))

    magic = u32(ee, 0x2E)
    ver = FW_MAGICS.get(magic, "UNKNOWN")
    print("FW magic       @0x2E : 0x%08X  (firmware %s)" % (magic, ver))

    print("param +2       @0x06 : 0x%04X  (default 0x0310)" % u16(ee, 0x06))
    print("param +14      @0x32 : 0x%04X  (default 0x0D01)" % u16(ee, 0x32))
    print("param +16      @0x34 : 0x%02X    (default 0x07)" % u8(ee, 0x34))
    print("param +18      @0x36 : 0x%04X  (default 0x0C4E)" % u16(ee, 0x36))

    esn = decode_esn(ee)
    print("\nESN            @0x0F : %r  (%s)" % (show_ascii(esn), esn.hex()))
    y, m, d = decode_date(ee)
    print("manufacture date     : year=%d (raw 0x%02X, ~%d)  month=%d  day=%d"
          % (y, y, 2000 + y, m, d))

    print("\nanti-replay t_now @0x21: 0x%08X" % u32(ee, 0x21))
    print("anti-replay t_ms  @0x25: 0x%08X" % u32(ee, 0x25))
    print("anti-replay t_to  @0x29: 0x%08X" % u32(ee, 0x29))

    # ---- Config block @ 0x08080C00 (offset 0xC00 within EEPROM) ----
    cb = 0xC00
    if not have_cfg:
        print("\n=== config block (0x08080C00): NOT in this dump — skipped ===")
        return
    print("\n=== config block (0x08080C00) ===")
    print("stored SOC      +0x24 : %d" % u32(ee, cb + 0x24))
    print("full-charge cap +0x28 : %d mAh   (0 => unprovisioned)" % u32(ee, cb + 0x28))
    print("remaining cap   +0x2C : %d mAh" % u32(ee, cb + 0x2C))
    print("CYCLE COUNT     +0x34 : %d" % u16(ee, cb + 0x34))
    print("RSOC %%          +0x36 : %d" % u8(ee, cb + 0x36))
    print("absolute SOC %%  +0x37 : %d" % u8(ee, cb + 0x37))
    print("volt thresh hi  +0x3A : %d (0x%04X)" % (u16(ee, cb + 0x3A), u16(ee, cb + 0x3A)))
    print("volt thresh lo  +0x3C : %d (0x%04X)" % (u16(ee, cb + 0x3C), u16(ee, cb + 0x3C)))
    print("phase markers   +0x44 : %r" % show_ascii(ee[cb + 0x44:cb + 0x47]))

    # ---- Raw dumps of the populated regions (calibration etc. visible here) ----
    def dump(lo, hi, label):
        print("\n--- raw %s (0x%08X-0x%08X) ---" % (label, EE_BASE + lo, EE_BASE + hi))
        for o in range(lo, hi, 16):
            row = ee[o:o + 16]
            print("0x%08X  %-47s  %s"
                  % (EE_BASE + o, " ".join("%02x" % x for x in row), show_ascii(row)))

    dump(0x00, 0x40, "low area (boot/ESN/date/cal/params)")
    dump(0xC00, 0xC60, "config block")
    dump(0xC80, 0xCC0, "secondary config")


if __name__ == "__main__":
    main()
