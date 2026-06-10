#!/usr/bin/env python3
"""Assemble a flashable VanMoof S3 BMS chip image from bmsboot + batteryware.

The battery module's STM32L072CZT6 carries three independently-flashed
regions. This tool lays them out at their real addresses and emits a single
combined Intel HEX (plus raw per-region bins) you can flash over SWD with
OpenOCD, STM32CubeProgrammer / ST-Link, or st-flash:

    0x08000000  bmsboot      (bootloader, <= 20 KB)        <- bmsboot.bin
    0x08005000  batteryware  (application / "AP" bank)     <- batteryware.bin
    0x08080000  data EEPROM  (6 KB, optional)              <- --eeprom / --gen-eeprom

bmsboot (exactly 0x5000 bytes in the OEM) ends precisely where the AP bank
begins, so the two are contiguous in main flash; the EEPROM is a separate
region 320 KB higher. The bootloader validates the AP image's VanMoof header
(magic 0xAA55AA55, CRC-32/MPEG-2, size < 0x15801) and, when valid, boots
AP_BASE+0x28; an all-zero EEPROM self-heals on first boot, so the EEPROM is
optional. Include one with --gen-eeprom (defaults matched to the app) or
--eeprom FILE (a prebuilt image or a dump from a working pack).

Usage:
    bms_build_image.py BMSBOOT.bin BATTERYWARE.bin [-o PREFIX]
        [--gen-eeprom | --eeprom EEPROM.bin]
        [--capacity MAH] [--esn ESN14] [--date YYYYMMDD] [--unprovisioned]

Outputs (PREFIX defaults to "bms_image"):
    PREFIX.hex          combined Intel HEX (all regions present)
    PREFIX_flash.bin    raw contiguous main flash (boot+app) -> 0x08000000
    PREFIX_eeprom.bin   raw EEPROM (only with an EEPROM) -> 0x08080000

Field offsets / map are from the bmsboot + batteryware decomps
(../docs/eeprom.md, ../docs/memory-map.md, ../../bmsboot/include/bmsboot.h).
"""
import argparse
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import eeprom_example  # same dir: build_eeprom(), EE_SIZE, MAGIC

# ---- BMS flash / EEPROM map (STM32L072CZT6; see bmsboot/include/bmsboot.h) ----
BOOT_BASE = 0x08000000      # bmsboot
APP_BASE = 0x08005000       # batteryware ("AP" bank), booted by bmsboot
SHADOW_BASE = 0x0801A800    # OTA-staging / golden backup (not written here)
EEPROM_BASE = 0x08080000    # STM32L0 on-chip data EEPROM
EEPROM_SIZE = eeprom_example.EE_SIZE       # 0x1800
BOOT_WINDOW = APP_BASE - BOOT_BASE         # 0x5000 (20 KB the loader owns)
BANK_SIZE = SHADOW_BASE - APP_BASE         # 0x15800 (86 KB per app bank)

IMG_MAGIC = 0xAA55AA55      # app header[0]
IMG_HDR_SIZE = 0x28         # header bytes before the vector table
IMG_MAX_SIZE = 0x15801      # bmsboot rejects an app whose size field is >= this
WARE_TYPE_BMS = 0xB1        # version-word low byte for batteryware

PAD_FLASH = 0xFF            # erased main-flash state
PAD_EEPROM = 0x00          # erased data-EEPROM state
POLY = 0x04C11DB7


def crc32_mpeg2_words(data):
    """MPEG-2 CRC32 over little-endian 32-bit words (the STM32 HW CRC unit).

    Identical routine to ../../tools/patch_image_header.py, verified byte-exact
    against the OEM batteryware/bmsboot images. `data` length must be a
    multiple of 4 (the firmware CRCs whole words).
    """
    crc = 0xFFFFFFFF
    for off in range(0, len(data), 4):
        crc ^= struct.unpack_from("<I", data, off)[0]
        for _ in range(32):
            crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


# --------------------------------------------------------------------------- #
# Intel HEX                                                                    #
# --------------------------------------------------------------------------- #
def _ihex_record(rectype, addr16, payload):
    body = bytes([len(payload), (addr16 >> 8) & 0xFF, addr16 & 0xFF, rectype]) + payload
    return ":%s%02X" % (body.hex().upper(), (-sum(body)) & 0xFF)


def build_ihex(segments, reclen=16):
    """Render segments [(base_addr, bytes), ...] as Intel HEX (type 04/00/01).

    Emits an Extended-Linear-Address record whenever the upper 16 bits of the
    address change, and never lets a data record straddle a 64 KB boundary.
    """
    lines = []
    cur_upper = None
    for base, blob in sorted(segments, key=lambda s: s[0]):
        pos = 0
        while pos < len(blob):
            addr = base + pos
            upper = (addr >> 16) & 0xFFFF
            if upper != cur_upper:
                lines.append(_ihex_record(0x04, 0, bytes([(upper >> 8) & 0xFF, upper & 0xFF])))
                cur_upper = upper
            n = min(reclen, len(blob) - pos, 0x10000 - (addr & 0xFFFF))
            lines.append(_ihex_record(0x00, addr & 0xFFFF, blob[pos:pos + n]))
            pos += n
    lines.append(":00000001FF")
    return "\n".join(lines) + "\n"


def parse_ihex(text):
    """Parse Intel HEX back to {addr: byte}. Used to self-check our output."""
    mem = {}
    upper = 0
    for line in text.splitlines():
        line = line.strip()
        if not line or not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        n, ahi, alo, rtype = raw[0], raw[1], raw[2], raw[3]
        data = raw[4:4 + n]
        if (-sum(raw[:-1])) & 0xFF != raw[-1]:
            raise ValueError("bad checksum: %s" % line)
        if rtype == 0x00:
            base = (upper << 16) | (ahi << 8) | alo
            for i, b in enumerate(data):
                mem[base + i] = b
        elif rtype == 0x04:
            upper = (data[0] << 8) | data[1]
        elif rtype == 0x01:
            break
    return mem


# --------------------------------------------------------------------------- #
# Validation                                                                   #
# --------------------------------------------------------------------------- #
class BuildError(Exception):
    pass


def _ver_name(word):
    return "%d.%02X.%d" % ((word >> 24) & 0xFF, (word >> 16) & 0xFF, (word >> 8) & 0xFF)


def load_boot(path, warn):
    data = open(path, "rb").read()
    if len(data) > BOOT_WINDOW:
        raise BuildError("bmsboot is 0x%X bytes but the boot window is only 0x%X "
                         "(it would overrun APP_BASE 0x%08X)."
                         % (len(data), BOOT_WINDOW, APP_BASE))
    if len(data) != BOOT_WINDOW:
        warn("bmsboot is 0x%X bytes; the OEM loader is exactly 0x%X. Padding the "
             "gap to APP_BASE with 0x%02X." % (len(data), BOOT_WINDOW, PAD_FLASH))
    if struct.unpack_from("<I", data, 0)[0] == IMG_MAGIC:
        raise BuildError("the bmsboot file starts with the app magic 0x%08X — that "
                         "looks like an application image, not the bootloader."
                         % IMG_MAGIC)
    sp, reset = struct.unpack_from("<II", data, 0)
    if not (0x20000000 <= sp <= 0x20005000):
        warn("bmsboot vector[0] (SP) = 0x%08X is outside SRAM 0x20000000-0x20005000 "
             "— is this really the bootloader?" % sp)
    if not (BOOT_BASE <= (reset & ~1) < APP_BASE) or not (reset & 1):
        warn("bmsboot vector[1] (Reset) = 0x%08X is not an odd address inside the "
             "boot window — is this really the bootloader?" % reset)
    if len(data) >= 8 and len(data) % 4 == 0:
        stored = struct.unpack_from("<I", data, len(data) - 4)[0]
        calc = crc32_mpeg2_words(data[:len(data) - 4])
        if stored != calc:
            warn("bmsboot trailing CRC 0x%08X != computed 0x%08X (image edited or "
                 "not finalized). It will still run from reset, but re-finalize with "
                 "patch_image_header.py if this is unexpected." % (stored, calc))
    return data


def load_app(path, warn):
    data = open(path, "rb").read()
    if len(data) < IMG_HDR_SIZE:
        raise BuildError("batteryware is shorter than the 0x%X-byte header." % IMG_HDR_SIZE)
    magic, version, stored_crc, size = struct.unpack_from("<IIII", data, 0)
    if magic != IMG_MAGIC:
        raise BuildError("batteryware header magic is 0x%08X, expected 0x%08X — not a "
                         "VanMoof application image." % (magic, IMG_MAGIC))
    if (version & 0xFF) != WARE_TYPE_BMS:
        warn("batteryware version word 0x%08X has type byte 0x%02X, not 0x%02X "
             "(batteryware). Are you flashing the right ware to the BMS?"
             % (version, version & 0xFF, WARE_TYPE_BMS))

    img_size = size
    if size != len(data):
        warn("header imageSize 0x%X != file length 0x%X. Using the header size for "
             "the CRC/extent (matches what bmsboot checks)." % (size, len(data)))
        if size > len(data):
            raise BuildError("header imageSize 0x%X exceeds the file (0x%X bytes) — "
                             "truncated image." % (size, len(data)))
    if img_size >= IMG_MAX_SIZE:
        raise BuildError("imageSize 0x%X >= 0x%X: bmsboot's image_verify() rejects this "
                         "(too large for the AP bank)." % (img_size, IMG_MAX_SIZE))
    if APP_BASE + len(data) > SHADOW_BASE:
        raise BuildError("the app (0x%X bytes at 0x%08X) would reach 0x%08X and overlap "
                         "the Shadow bank at 0x%08X."
                         % (len(data), APP_BASE, APP_BASE + len(data), SHADOW_BASE))
    if img_size % 4 == 0:
        body = bytearray(data[:img_size])
        body[0x08:0x10] = b"\xff" * 8          # blank crc32 + imageSize, as the firmware does
        calc = crc32_mpeg2_words(bytes(body))
        if calc != stored_crc:
            warn("batteryware header CRC 0x%08X != computed 0x%08X. bmsboot WILL REFUSE "
                 "this image and fall back to Shadow/serial download. Finalize it first: "
                 "python3 ../../tools/patch_image_header.py %s" % (stored_crc, calc, path))
    else:
        warn("batteryware imageSize 0x%X is not word-aligned; skipping the header-CRC "
             "check (the firmware CRCs whole words)." % img_size)
    return data, version


def load_eeprom(path, warn):
    data = bytearray(open(path, "rb").read())
    if len(data) > EEPROM_SIZE:
        raise BuildError("EEPROM image is 0x%X bytes; the data EEPROM is only 0x%X."
                         % (len(data), EEPROM_SIZE))
    if len(data) < EEPROM_SIZE:
        warn("EEPROM image is 0x%X bytes; padding to 0x%X with 0x%02X (erased)."
             % (len(data), EEPROM_SIZE, PAD_EEPROM))
        data += bytes([PAD_EEPROM]) * (EEPROM_SIZE - len(data))
    if data[0] in (0xCC, 0x33, 0x5A):
        warn("EEPROM boot flag @0x08080000 is 0x%02X (a recover/ack/wipe trigger). A "
             "clean image should hold 0x55 (normal) or 0x00 (erased)." % data[0])
    return bytes(data)


# --------------------------------------------------------------------------- #
# CLI                                                                          #
# --------------------------------------------------------------------------- #
def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Assemble a flashable VanMoof S3 BMS image (bmsboot + batteryware "
                    "[+ EEPROM]).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("bmsboot", help="bootloader binary -> 0x08000000")
    p.add_argument("batteryware", help="application binary -> 0x08005000")
    p.add_argument("-o", "--out", default="bms_image", help="output prefix (default bms_image)")

    g = p.add_mutually_exclusive_group()
    g.add_argument("--eeprom", metavar="FILE",
                   help="include a prebuilt 6 KB EEPROM image at 0x08080000")
    g.add_argument("--gen-eeprom", action="store_true",
                   help="generate a default EEPROM (FW-magic matched to the app) and include it")

    p.add_argument("--capacity", type=int, default=12600,
                   help="--gen-eeprom: full-charge capacity in mAh (default 12600)")
    p.add_argument("--esn", help="--gen-eeprom: 14-char electronic serial number")
    p.add_argument("--date", help="--gen-eeprom: manufacture date YYYYMMDD")
    p.add_argument("--unprovisioned", action="store_true",
                   help="--gen-eeprom: leave the gauge unprovisioned (firmware factory-inits)")
    p.add_argument("--reclen", type=int, default=16,
                   help="Intel HEX data bytes per record (default 16)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    warnings = []
    warn = lambda m: warnings.append(m)

    try:
        boot = load_boot(args.bmsboot, warn)
        app, app_version = load_app(args.batteryware, warn)
        eeprom = None
        if args.eeprom:
            eeprom = load_eeprom(args.eeprom, warn)
        elif args.gen_eeprom:
            eeprom = bytes(eeprom_example.build_eeprom(
                fw_magic=app_version, capacity=args.capacity, esn=args.esn,
                date=args.date, provisioned=not args.unprovisioned))
    except BuildError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1

    # ---- assemble ----
    segments = [(BOOT_BASE, boot), (APP_BASE, app)]
    if eeprom is not None:
        segments.append((EEPROM_BASE, eeprom))

    hex_text = build_ihex(segments, reclen=args.reclen)
    # Self-check: parse our own HEX back and confirm every region byte matches.
    mem = parse_ihex(hex_text)
    for base, blob in segments:
        for i, b in enumerate(blob):
            if mem.get(base + i) != b:
                print("error: internal Intel HEX self-check failed at 0x%08X"
                      % (base + i), file=sys.stderr)
                return 1

    # raw contiguous main flash (boot padded to the window, then app)
    flash = bytearray(boot)
    flash += bytes([PAD_FLASH]) * (BOOT_WINDOW - len(flash))
    flash += app

    hex_path = args.out + ".hex"
    flash_path = args.out + "_flash.bin"
    open(hex_path, "w").write(hex_text)
    open(flash_path, "wb").write(flash)
    eeprom_path = None
    if eeprom is not None:
        eeprom_path = args.out + "_eeprom.bin"
        open(eeprom_path, "wb").write(eeprom)

    _report(args, boot, app, app_version, eeprom, warnings,
            hex_path, flash_path, eeprom_path)
    return 0


def _report(args, boot, app, app_version, eeprom, warnings,
            hex_path, flash_path, eeprom_path):
    app_end = APP_BASE + len(app)
    print("VanMoof S3 BMS image — STM32L072CZT6\n")
    print("  region       address                    size       source")
    print("  bmsboot      0x%08X-0x%08X    0x%05X    %s"
          % (BOOT_BASE, BOOT_BASE + len(boot) - 1, len(boot), os.path.basename(args.bmsboot)))
    print("  batteryware  0x%08X-0x%08X    0x%05X    %s  (v%s)"
          % (APP_BASE, app_end - 1, len(app), os.path.basename(args.batteryware),
             _ver_name(app_version)))
    if eeprom is not None:
        src = os.path.basename(args.eeprom) if args.eeprom else "generated"
        print("  data EEPROM  0x%08X-0x%08X    0x%05X    %s"
              % (EEPROM_BASE, EEPROM_BASE + len(eeprom) - 1, len(eeprom), src))
    print("  free in AP bank: 0x%X bytes (Shadow backup begins at 0x%08X)"
          % (SHADOW_BASE - app_end, SHADOW_BASE))

    print("\nwrote:")
    print("  %-22s combined Intel HEX (all regions; for OpenOCD / CubeProgrammer)" % hex_path)
    print("  %-22s raw main flash -> flash at 0x%08X (boot+app, st-flash)"
          % (flash_path, BOOT_BASE))
    if eeprom_path:
        print("  %-22s raw EEPROM -> flash at 0x%08X" % (eeprom_path, EEPROM_BASE))

    if warnings:
        print("\nwarnings:")
        for w in warnings:
            print("  ! %s" % w)

    print("\nflash it (pick one tool):")
    print("  # STM32CubeProgrammer / ST-Link — handles main flash AND the L0 data EEPROM:")
    print("      STM32_Programmer_CLI -c port=SWD -d %s -v -rst" % hex_path)
    print("  # OpenOCD (ST-Link/J-Link):")
    print("      openocd -f interface/stlink.cfg -f target/stm32l0.cfg \\")
    print("              -c \"program %s verify reset exit\"" % hex_path)
    if eeprom_path:
        print("    # if your OpenOCD build won't write the 0x%08X EEPROM from the .hex,"
              % EEPROM_BASE)
        print("    # program it separately with CubeProgrammer:")
        print("      STM32_Programmer_CLI -c port=SWD -d %s 0x%08X -v"
              % (eeprom_path, EEPROM_BASE))
    print("  # st-flash (open-source stlink) — main flash only, NOT the data EEPROM:")
    print("      st-flash write %s 0x%08X" % (flash_path, BOOT_BASE))
    if eeprom_path:
        print("      # then the EEPROM via CubeProgrammer/OpenOCD (st-flash can't reach 0x%08X)"
              % EEPROM_BASE)
    print("\nnote: a locked pack may have flash read-out protection (RDP) — unlocking it "
          "mass-erases the chip (EEPROM identity included). Don't mass-erase a pack whose "
          "ESN/calibration you want to keep; flashing this image only writes the regions above.")


if __name__ == "__main__":
    raise SystemExit(main())
