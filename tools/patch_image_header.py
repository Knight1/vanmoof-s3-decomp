#!/usr/bin/env python3
"""Finalise a VanMoof firmware's checksum in a raw .bin.

Shared across every VanMoof S3 firmware. Three formats are handled,
auto-detected from the leading magic (the same dispatch as
chwdt/vanmoof-tools `crc32.c`):

1. APPLICATION ware — 40-byte front header, magic 0xAA55AA55 (batteryware,
   shifterware, mainware, …):

       +0x00  uint32   magic      0xAA55AA55
       +0x04  uint32   version    MAJOR.MINOR.PATCH.TYPE (1 byte each, MSB-first)
       +0x08  uint32   crc32      <- patched here
       +0x0C  uint32   imageSize  <- patched here (total image length)
       +0x10  char[12] build date (ASCII, NUL-terminated)
       +0x1C  char[12] build time (ASCII, NUL-terminated)

   CRC = MPEG-2 CRC32 over the whole image with the crc and imageSize fields
   (header bytes [8:16)) blanked to 0xFFFFFFFF first.

2. BOOTLOADER binary — no front header (raw vector table first); a checksum
   trailer occupies the last 8 bytes (shifterboot, mainboot, bmsboot):

       [len-8 : len-4]  uint32  version  (build id; not modified)
       [len-4 : len  ]  uint32  crc32    <- patched here

   CRC = MPEG-2 CRC32 over the whole file except the trailing 4-byte CRC slot,
   i.e. crc32(image[0 : len-4]). Matches crc32.c's "assume boot-loader binary"
   path (crc32_calculate over st_size - sizeof(uint32_t)).

3. BLE ware — TI OAD-NVM1 header, magic "OAD NVM1" (bleware, on the CC2642):

       +0x00  char[8]  imgID  "OAD NVM1"
       +0x08  uint32   crc32  <- patched here
       +0x11  uint8    crcStat   (must be 0xFF at build time; covered by the CRC)
       +0x18  uint32   len       (total image length incl. header; build-set)
       +0x1C  uint32   prgEntry  (post-header offset, normally 0x90)
       +0x20  uint32   softVer

   CRC = zlib/ISO-HDLC CRC32 (poly 0xEDB88320 reflected, init+xorout 0xFFFFFFFF)
   over image[12 : len] — i.e. everything after the imgID+crc fields, through
   the header `len`. This is a DIFFERENT CRC than formats 1/2. Matches crc32.c's
   BLE branch: crc32(0, data + 12, le32toh(ble_ware.len) - 12). `len` is trusted
   from the header (validated <= file size), not rewritten.

Formats 1 and 2 use the MPEG-2 CRC32 (poly 0x04C11DB7, init 0xFFFFFFFF, no
reflection, over little-endian 32-bit words) the STM32/MM32 hardware CRC unit
computes; format 3 uses the standard zlib CRC-32. Verified byte-exact against
the OEM batteryware (1.17.1, 1.14.1) and shifterware (0.237) images and against
chwdt/vanmoof-tools `crc32.c` (crc32_calculate / ware_crc / the BLE branch).

motorware (TMS320F28054, C2000) has no dedicated handling — it has neither magic
and would fall into the bootloader path, which is not known to be correct for it.

Usage: patch_image_header.py <image.bin> [<image2.bin> ...]
Exit:  0 if every image patched; 1 if any failed (the rest are still patched).
"""
import struct
import sys
import zlib

WARE_MAGIC = 0xAA55AA55
BLE_MAGIC = b"OAD NVM1"
POLY = 0x04C11DB7
INIT = 0xFFFFFFFF
HEADER_SIZE = 0x28
BLE_MIN_HEADER = 0x2C       # core OAD-NVM1 header (hdrLen)


def crc32_mpeg2_words(data: bytes) -> int:
    """MPEG-2 CRC32 over little-endian 32-bit words (the STM32/MM32 HW CRC).

    Equivalent to crc32.c's crc32_calculate: `crc ^= *p++` per 32-bit LE word,
    then 32 MSB-first shift/xor rounds.
    """
    crc = INIT
    for off in range(0, len(data), 4):
        crc ^= struct.unpack_from("<I", data, off)[0]
        for _ in range(32):
            crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def patch_ware(path: str, data: bytearray) -> bool:
    """Application ware: set imageSize, then CRC with [8:16) blanked."""
    if len(data) < HEADER_SIZE:
        sys.stderr.write("error: %s: shorter than the %d-byte header\n"
                         % (path, HEADER_SIZE))
        return False

    old_size = struct.unpack_from("<I", data, 0x0C)[0]
    if old_size != len(data):
        sys.stderr.write("note: %s: imageSize 0x%08X -> 0x%08X (file length)\n"
                         % (path, old_size, len(data)))
    struct.pack_into("<I", data, 0x0C, len(data))

    body = bytearray(data)
    body[8:16] = b"\xff" * 8
    crc = crc32_mpeg2_words(bytes(body))
    struct.pack_into("<I", data, 0x08, crc)

    open(path, "wb").write(data)
    version = struct.unpack_from("<I", data, 0x04)[0]
    print("patched %s [ware]: version=0x%08X imageSize=%d (0x%X) crc32=0x%08X"
          % (path, version, len(data), len(data), crc))
    return True


def patch_bootloader(path: str, data: bytearray) -> bool:
    """Bootloader binary: CRC over [0 : len-4], stored in the last 4 bytes."""
    if len(data) < 8:
        sys.stderr.write("error: %s: too short for a bootloader CRC trailer\n" % path)
        return False

    crc = crc32_mpeg2_words(bytes(data[:len(data) - 4]))
    struct.pack_into("<I", data, len(data) - 4, crc)

    open(path, "wb").write(data)
    ver = data[len(data) - 8:len(data) - 4]                 # version word
    vchars = "".join(chr(b) if 32 <= b < 127 else "." for b in (ver[3], ver[2], ver[1]))
    print("patched %s [bootloader]: version=%r crc32=0x%08X (trailer at 0x%X)"
          % (path, vchars, crc, len(data) - 4))
    return True


def patch_ble(path: str, data: bytearray) -> bool:
    """TI OAD-NVM1 ware: zlib CRC32 over [12 : len], stored at +0x08.

    `len` (+0x18) is the header's image length; it is trusted, not rewritten.
    The CRC covers the crcStat byte (+0x11), which must be 0xFF at build time —
    exactly its state in a freshly-built (unpromoted) image.
    """
    if len(data) < BLE_MIN_HEADER:
        sys.stderr.write("error: %s: shorter than the OAD-NVM1 core header\n" % path)
        return False

    length = struct.unpack_from("<I", data, 0x18)[0]
    if length <= 12:
        sys.stderr.write("error: %s: OAD len 0x%08X too small\n" % (path, length))
        return False
    if length > len(data):
        sys.stderr.write("error: %s: OAD len 0x%08X exceeds file size 0x%08X\n"
                         % (path, length, len(data)))
        return False

    crc = zlib.crc32(bytes(data[12:length])) & 0xFFFFFFFF
    struct.pack_into("<I", data, 0x08, crc)

    open(path, "wb").write(data)
    soft = struct.unpack_from("<I", data, 0x20)[0]
    print("patched %s [ble]: softVer=0x%08X len=0x%X crc32=0x%08X"
          % (path, soft, length, crc))
    return True


def patch(path: str) -> bool:
    """Patch one firmware image in place, auto-selecting the format."""
    try:
        data = bytearray(open(path, "rb").read())
    except OSError as exc:
        sys.stderr.write("error: %s: %s\n" % (path, exc))
        return False

    if len(data) < 8:
        sys.stderr.write("error: %s: too short to be a firmware image\n" % path)
        return False

    if data[:8] == BLE_MAGIC:
        return patch_ble(path, data)

    # MPEG-2 formats require word alignment (they iterate 32-bit words).
    if len(data) % 4 != 0:
        sys.stderr.write("error: %s: length %d is not a multiple of 4\n"
                         % (path, len(data)))
        return False

    if struct.unpack_from("<I", data, 0x00)[0] == WARE_MAGIC:
        return patch_ware(path, data)

    # No 0xAA55AA55 / "OAD NVM1" — mirror crc32.c and assume a bootloader binary.
    sys.stderr.write("note: %s: no 0x%08X magic; treating as bootloader binary "
                     "(trailing CRC)\n" % (path, WARE_MAGIC))
    return patch_bootloader(path, data)


def main(argv) -> int:
    if len(argv) < 2:
        sys.stderr.write("usage: patch_image_header.py <image.bin> [<image2.bin> ...]\n")
        return 2
    ok = True
    for path in argv[1:]:
        ok = patch(path) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
