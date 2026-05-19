#!/usr/bin/env python3
"""Patch the VanMoof image-header CRC32 field in a built shifterware bin.

Run as a post-objcopy step (see Makefile). The image header layout is:

    +0x00  magic    uint32_t   0xAA55AA55
    +0x04  version  uint32_t   build/version id
    +0x08  crc      uint32_t   << patched by this tool
    +0x0C  length   uint32_t   image size in bytes (set by linker)
    +0x10  date     char[12]
    +0x1C  time     char[12]   (9 chars + 3-byte 0xFF pad)

The CRC algorithm matches shifterboot's validation (per
https://github.com/chwdt/vanmoof-tools/blob/master/crc32.c): MPEG-2-style
CRC32 with polynomial 0x4C11DB7, initial value 0xFFFFFFFF, no input or
output reflection. The hash is computed over the whole image with the
`crc` and `length` fields both replaced by 0xFFFFFFFF — i.e. on the
*pre-patch* state. The polynomial matches the STM32/MM32 hardware CRC
peripheral exactly, so on-device validation can use the silicon CRC
engine.

Usage:
    patch_image_crc.py <path-to-bin>

Exit codes:
    0  CRC patched in place
    1  bad header (magic mismatch, length out of range, etc.)
"""

import struct
import sys
from pathlib import Path

MAGIC = 0xAA55AA55
POLY = 0x4C11DB7
INIT = 0xFFFFFFFF
HEADER_SIZE = 40


def crc32_mpeg2(crc, data):
    """Bit-serial MPEG-2 CRC32 over a word-aligned buffer."""
    if len(data) % 4 != 0:
        raise ValueError(f"data length {len(data)} not a multiple of 4")
    for i in range(0, len(data), 4):
        word = struct.unpack_from("<I", data, i)[0]
        crc ^= word
        for _ in range(32):
            if crc & (1 << 31):
                crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <bin>", file=sys.stderr)
        return 1

    path = Path(sys.argv[1])
    data = bytearray(path.read_bytes())

    magic, version, stored_crc, length = struct.unpack("<IIII", data[:16])
    if magic != MAGIC:
        print(f"{path}: header magic 0x{magic:08X} != 0x{MAGIC:08X}", file=sys.stderr)
        return 1
    if length != len(data):
        print(
            f"{path}: header length 0x{length:08X} != file size 0x{len(data):08X}",
            file=sys.stderr,
        )
        return 1
    if length % 4 != 0:
        print(f"{path}: length 0x{length:08X} not word-aligned", file=sys.stderr)
        return 1

    # Compute CRC over the image with crc and length both blanked out.
    tmp = bytearray(data[:HEADER_SIZE])
    struct.pack_into("<I", tmp, 8, 0xFFFFFFFF)   # crc
    struct.pack_into("<I", tmp, 12, 0xFFFFFFFF)  # length
    crc = INIT
    crc = crc32_mpeg2(crc, bytes(tmp))
    crc = crc32_mpeg2(crc, bytes(data[HEADER_SIZE:length]))

    # Patch the file in place.
    struct.pack_into("<I", data, 8, crc)
    path.write_bytes(bytes(data))

    print(
        f"{path}: patched CRC 0x{stored_crc:08X} -> 0x{crc:08X} "
        f"(version 0x{version:08X}, length 0x{length:08X})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
