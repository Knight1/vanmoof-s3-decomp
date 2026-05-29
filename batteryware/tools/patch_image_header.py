#!/usr/bin/env python3
"""Finalise the VanMoof batteryware image header in a raw .bin.

The 40-byte VanMoof image header (see src/startup_stm32l072.S) carries two
fields that can only be filled in after the final image size is known:

  +0x08  uint32  CRC32   (image checksum, gates execution in bmsboot)
  +0x0C  uint32  imageSize (total image length in bytes)

Algorithm (reverse-engineered from batteryware's own flash_verify_header @
runtime 0x0800B340, verified byte-exact against the OEM 1.17.1 and 1.14.1
images): the CRC is the STM32L0 hardware CRC unit's result — poly 0x04C11DB7,
init 0xFFFFFFFF, no input/output bit reversal, no final XOR — fed the image as
little-endian 32-bit words, computed over the whole image [0 : imageSize] with
the CRC and imageSize header fields (bytes [8:16)) forced to 0xFFFFFFFF first.
Feeding little-endian words to that big-endian-order unit is equivalent to
byte-swapping each 32-bit word and running CRC32/MPEG-2 over the byte stream.

Usage: patch_image_header.py <image.bin>
"""
import sys
import struct


def crc32_mpeg2(buf: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in buf:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def image_crc(image: bytes) -> int:
    """STM32 hardware CRC over the image, with the CRC+imageSize fields masked."""
    body = bytearray(image)
    body[8:16] = b"\xff" * 8                       # CRC + imageSize -> 0xFFFFFFFF
    words = struct.unpack(">%dI" % (len(body) // 4), bytes(body))   # big-endian read
    swapped = struct.pack("<%dI" % len(words), *words)              # == byteswap each LE word
    return crc32_mpeg2(swapped)


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: patch_image_header.py <image.bin>\n")
        return 2
    path = sys.argv[1]
    data = bytearray(open(path, "rb").read())

    if len(data) < 0x28:
        sys.stderr.write("error: image shorter than the 40-byte header\n")
        return 1
    if len(data) % 4 != 0:
        sys.stderr.write("error: image length %d is not a multiple of 4\n" % len(data))
        return 1
    if struct.unpack_from("<I", data, 0)[0] != 0xAA55AA55:
        sys.stderr.write("error: bad image magic (not a VanMoof image)\n")
        return 1

    # imageSize = total length; the linker already sets this, but make it
    # authoritative here so the field always matches the emitted file.
    struct.pack_into("<I", data, 0x0C, len(data))
    crc = image_crc(data)
    struct.pack_into("<I", data, 0x08, crc)

    open(path, "wb").write(data)
    print("patched %s: imageSize=%d (0x%X), crc32=0x%08X"
          % (path, len(data), len(data), crc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
