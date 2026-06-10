#!/usr/bin/env python3
"""Generate a COMPLETE VanMoof S3 BMS data-EEPROM image (6 KB).

Produces a known-good 0x1800-byte image for the STM32L072 on-chip data
EEPROM at 0x08080000. It covers *every* field both halves of the BMS
firmware persist there:

  * the **bmsboot** bootloader's view  — the persisted boot flag at
    0x08080000 and the reset-cause word at 0x08080002; and
  * the **batteryware** application's view — boot mode, validated config
    params, FW-magic, ESN/manufacture date, anti-replay ticks, and the
    live config block (SOC, capacity, cycle count, voltage thresholds,
    phase markers).

Field offsets come from the 1.17.1 decomp (see ../docs/eeprom.md) and the
bmsboot decomp (../../bmsboot/include/bmsboot.h) — confirm against your
firmware. The result is a full image you can flash straight to 0x08080000,
or fold into a complete chip image with `bms_build_image.py --eeprom ...`.

The firmware self-heals a lot: an ALL-ZERO EEPROM already boots safely and
factory-inits (FW-magic mismatch rewrites defaults; provisioned-flag 0 runs
factory init). This generator writes explicit defaults on top of that so the
image is human-readable, the version-magic matches, and the bootloader sees a
clean "normal boot" flag instead of the erased 0x00.

What this CANNOT recover (pack-specific, must be redone on the pack):
  * ESN + manufacture date  -> write with the PBU "WriteESNAndDate" (Modbus
    func 0x10, regs 0x0C-0x14) after flashing, unless you pass --esn/--date.
  * CHG / DSG current calibration -> re-run the "CHG CAL" / "DSG CAL" console
    commands; the gauge is inaccurate until then.
  * Learned SOC / capacity / cycle count -> reset; the gauge re-learns.

Usage:
    eeprom_example.py [--version 1.14.1|1.17.1] [--capacity MAH]
                      [--esn ESN14] [--date YYYYMMDD]
                      [--boot-flag HEX] [--unprovisioned] [-o out.bin]

Importable: `build_eeprom(...)` returns the 0x1800-byte bytearray; this is
what bms_build_image.py uses to fold a default EEPROM into a chip image.
"""
import argparse
import struct
import sys

EE_SIZE = 0x1800

# FW-magic / header version word, keyed by VanMoof's hex version name. The
# value is identical to the batteryware image-header version word, so the
# image builder can pass an app's real header word straight through.
MAGIC = {"1.14.1": 0x011401B1, "1.17.1": 0x011701B1}

# bmsboot persisted boot flag (BOOT_FLAG_ADDR 0x08080000). 0x55 = "normal:
# validate + boot AP". 0xCC/0x33/0x5A are the recover/ack/wipe triggers and
# must NOT be used for a clean image. 0x00 (erased) also boots normally.
BOOT_FLAG_NORMAL = 0x55

# Validated config defaults (config_init fallbacks; version-independent in the
# decoded firmware). config_init reverts to these on FW-magic mismatch.
DEF_PARAM_06 = 0x0310
DEF_PARAM_32 = 0x0D01
DEF_PARAM_34 = 0x07
DEF_PARAM_36 = 0x0C4E

# config block @ 0x08080C00 defaults
CFG_BASE = 0xC00
DEF_VTHRESH = 1000            # 0x03E8; must stay in (900, 1099]
DEF_ABS_SOC = 100             # absolute-SOC full-scale reference
PHASE_MARKER = ord("A")       # 0x41


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


def build_eeprom(fw_magic=MAGIC["1.17.1"], capacity=12600, esn=None, date=None,
                 boot_flag=BOOT_FLAG_NORMAL, boot_mode=0x00, provisioned=True):
    """Return a complete 0x1800-byte BMS EEPROM image (erased = 0x00).

    fw_magic    : FW-magic word @0x2E (== the app's header version word).
    capacity    : full-charge capacity (mAh) @0x0C28; doubles as the
                  provisioned flag (non-zero => skip factory init).
    esn / date  : optional 14-char serial / YYYYMMDD manufacture date.
    boot_flag   : bmsboot flag @0x00 (0x55 normal). NEVER 0xCC/0x33/0x5A.
    boot_mode   : batteryware boot mode @0x01 (0 normal). NEVER 0x17/0x18.
    provisioned : when False, force capacity 0 so the firmware factory-inits.
    """
    ee = bytearray(EE_SIZE)

    # ---- bmsboot view (loader reads these before handing off to the app) ----
    ee[0x00] = boot_flag & 0xFF           # boot flag: 0x55 = normal boot AP
    # 0x02-0x05 saved RCC_CSR reset cause -> 0 (the loader rewrites it each boot)

    # ---- batteryware low area: boot / identity / validated params ----
    ee[0x01] = boot_mode & 0xFF           # boot mode: normal (NEVER 0x17/0x18)
    put_u16(ee, 0x06, DEF_PARAM_06)       # validated param default
    put_u32(ee, 0x2E, fw_magic)           # FW magic -> matches the flashed app
    put_u16(ee, 0x32, DEF_PARAM_32)
    ee[0x34] = DEF_PARAM_34
    put_u16(ee, 0x36, DEF_PARAM_36)
    # anti-replay triplet @0x21/0x25/0x29 left 0 -> next ESN/date write commits.

    if esn:
        encode_esn(ee, esn)
    if date:
        encode_date(ee, date)

    # ---- config block @ 0x08080C00 (live BMS state) ----
    cb = CFG_BASE
    put_u32(ee, cb + 0x24, 0)                          # stored SOC (re-learned)
    put_u32(ee, cb + 0x28, capacity if provisioned else 0)  # full-charge cap / provisioned flag
    put_u32(ee, cb + 0x2C, 0)                          # remaining capacity
    put_u16(ee, cb + 0x34, 0)                          # cycle count
    ee[cb + 0x36] = 0                                  # RSOC %
    ee[cb + 0x37] = DEF_ABS_SOC                        # absolute SOC full-ref
    put_u16(ee, cb + 0x3A, DEF_VTHRESH)                # voltage threshold hi
    put_u16(ee, cb + 0x3C, DEF_VTHRESH)                # voltage threshold lo
    put_u16(ee, cb + 0x40, 0)                          # factory-zeroed
    put_u16(ee, cb + 0x42, 0)                          # factory-zeroed
    ee[cb + 0x44] = PHASE_MARKER                       # per-phase voltage markers
    ee[cb + 0x45] = PHASE_MARKER
    ee[cb + 0x46] = PHASE_MARKER
    # secondary config @ 0x08080C80 left all-zero (factory-init state).

    return ee


def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Generate a complete VanMoof S3 BMS data-EEPROM image (6 KB).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--version", choices=sorted(MAGIC), default="1.17.1",
                   help="firmware version whose FW-magic to stamp (default 1.17.1)")
    p.add_argument("--capacity", type=int, default=12600,
                   help="full-charge capacity in mAh (default 12600)")
    p.add_argument("--esn", help="14-char electronic serial number")
    p.add_argument("--date", help="manufacture date YYYYMMDD")
    p.add_argument("--boot-flag", default=hex(BOOT_FLAG_NORMAL),
                   help="bmsboot boot flag @0x08080000 (default 0x55 normal)")
    p.add_argument("--unprovisioned", action="store_true",
                   help="leave capacity 0 so the firmware factory-inits the config block")
    p.add_argument("-o", "--out", default="eeprom_example.bin",
                   help="output file (default eeprom_example.bin)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)

    boot_flag = int(args.boot_flag, 0)
    if boot_flag in (0xCC, 0x33, 0x5A):
        print("error: --boot-flag 0x%02X is a recover/ack/wipe trigger; use 0x55 "
              "(normal) or 0x00 (erased)." % boot_flag, file=sys.stderr)
        return 2

    ee = build_eeprom(fw_magic=MAGIC[args.version], capacity=args.capacity,
                      esn=args.esn, date=args.date, boot_flag=boot_flag,
                      provisioned=not args.unprovisioned)

    with open(args.out, "wb") as f:
        f.write(ee)

    print("wrote %s  (0x%X bytes, firmware %s, capacity %d mAh%s)"
          % (args.out, EE_SIZE, args.version,
             0 if args.unprovisioned else args.capacity,
             ", UNPROVISIONED (firmware factory-inits)" if args.unprovisioned else ""))
    print("boot flag @0x08080000 : 0x%02X   boot mode @0x08080001 : 0x00" % boot_flag)
    print("ESN  : %s" % (args.esn or "(left blank - set via PBU WriteESNAndDate)"))
    print("date : %s" % (args.date or "(left blank - set via PBU WriteESNAndDate)"))
    print("\nFlash to 0x08080000 (or fold into a chip image with "
          "bms_build_image.py --eeprom %s), then on the pack:" % args.out)
    print("  1. PBU WriteESNAndDate  (sets serial + manufacture date)")
    print("  2. CHG CAL / DSG CAL    (re-calibrate charge/discharge current)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
