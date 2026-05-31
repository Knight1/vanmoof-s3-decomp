#!/usr/bin/env python3
"""Finalise the VanMoof image header (CRC32 + imageSize) in a raw .bin.

Shared across every VanMoof S3 firmware that carries the standard 40-byte
STM32/MM32 image header — batteryware, shifterware, mainware and siblings.
(bleware uses a TI OAD-NVM1 header and motorware a C2000 layout; both are
rejected by the magic check, so pointing this tool at them fails cleanly.)

Header layout (little-endian):

    +0x00  uint32   magic      0xAA55AA55
    +0x04  uint32   version    MAJOR.MINOR.PATCH.TYPE (1 byte each, MSB-first)
    +0x08  uint32   crc32      <- patched by this tool
    +0x0C  uint32   imageSize  <- patched by this tool (total image length)
    +0x10  char[12] build date (ASCII, NUL-terminated)
    +0x1C  char[12] build time (ASCII, NUL-terminated)

CRC algorithm: MPEG-2 CRC32 — polynomial 0x04C11DB7, init 0xFFFFFFFF, no input
or output reflection, no final XOR — over the whole image as little-endian
32-bit words, with the crc and imageSize fields (header bytes [8:16)) blanked
to 0xFFFFFFFF for the calculation. This is exactly what the STM32/MM32 hardware
CRC peripheral computes, so each firmware's bootloader validates the image on
the silicon engine. Verified byte-exact against the OEM batteryware (1.17.1,
1.14.1) and shifterware (0.237) images, and confirmed against
chwdt/vanmoof-tools/crc32.c.

Usage: patch_image_header.py <image.bin> [<image2.bin> ...]
Exit:  0 if every image patched; 1 if any image failed (others still patched).
"""
import struct
import sys

MAGIC = 0xAA55AA55
POLY = 0x04C11DB7
INIT = 0xFFFFFFFF
HEADER_SIZE = 0x28


def crc32_mpeg2_words(data: bytes) -> int:
    """MPEG-2 CRC32 over little-endian 32-bit words (the STM32/MM32 HW CRC).

    Processing a 32-bit word MSB-first is identical to byte-swapping the word
    and running a byte-serial MSB-first CRC over the bytes — both prior per-ware
    scripts did one or the other; this is the same result.
    """
    crc = INIT
    for off in range(0, len(data), 4):
        crc ^= struct.unpack_from("<I", data, off)[0]
        for _ in range(32):
            crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def patch(path: str) -> bool:
    """Patch one image's header in place. Returns True on success."""
    try:
        data = bytearray(open(path, "rb").read())
    except OSError as exc:
        sys.stderr.write("error: %s: %s\n" % (path, exc))
        return False

    if len(data) < HEADER_SIZE:
        sys.stderr.write("error: %s: shorter than the %d-byte header\n"
                         % (path, HEADER_SIZE))
        return False
    if len(data) % 4 != 0:
        sys.stderr.write("error: %s: length %d is not a multiple of 4\n"
                         % (path, len(data)))
        return False

    magic = struct.unpack_from("<I", data, 0x00)[0]
    if magic != MAGIC:
        sys.stderr.write(
            "error: %s: header magic 0x%08X != 0x%08X "
            "(not a VanMoof STM32/MM32 image; bleware/motorware use other headers)\n"
            % (path, magic, MAGIC))
        return False

    # imageSize = total length. The linker normally sets this; make it
    # authoritative so the field always matches the emitted file.
    old_size = struct.unpack_from("<I", data, 0x0C)[0]
    if old_size != len(data):
        sys.stderr.write("note: %s: imageSize 0x%08X -> 0x%08X (file length)\n"
                         % (path, old_size, len(data)))
    struct.pack_into("<I", data, 0x0C, len(data))

    # CRC over the image with crc + imageSize (bytes [8:16)) blanked.
    body = bytearray(data)
    body[8:16] = b"\xff" * 8
    crc = crc32_mpeg2_words(bytes(body))
    struct.pack_into("<I", data, 0x08, crc)

    open(path, "wb").write(data)
    version = struct.unpack_from("<I", data, 0x04)[0]
    print("patched %s: version=0x%08X imageSize=%d (0x%X) crc32=0x%08X"
          % (path, version, len(data), len(data), crc))
    return True


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
