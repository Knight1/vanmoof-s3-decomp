#ifndef SHIFTERBOOT_IMAGE_H
#define SHIFTERBOOT_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Halfword-aligned stream helpers used by `main` while parsing the
 * VanMoof image header during OTA verification. Cortex-M0 only
 * supports 16-bit-aligned `ldrh` and 32-bit-aligned `ldr`, so the
 * OEM does u32 reads as two `ldrh`s OR'd together via a halfword
 * staging buffer — making this set of helpers tolerant of sources
 * that are only 16-bit aligned. */

uint16_t image_read_halfword(const uint16_t *p);
void     image_copy_halfwords(const uint16_t *src,
                              uint16_t *dst,
                              uint16_t count);
uint32_t image_read_u32_le(const uint16_t *src,
                           uint16_t *staging);

/* VanMoof image-header magic. Stored at offset `+0x00` of every
 * OTA-distributable VanMoof firmware image. */
#define IMAGE_MAGIC   0xAA55AA55u

/* Maximum permitted image size — 12 KB. The OEM materialises this
 * as `movs r1, #3; lsls r1, #0xC` (= 0x3000). Matches the gap
 * between slot 1 (`0x08001800`) and slot 2 (`0x08004800`). */
#define IMAGE_MAX_SIZE  0x3000u

/* Size of the VanMoof image header in bytes (40). */
#define IMAGE_HDR_SIZE  0x28u

/* Status codes returned by `image_verify_crc`. */
#define IMAGE_OK           0  /* magic, length, and CRC all valid */
#define IMAGE_CRC_MISMATCH 1  /* CRC stored in header != computed CRC */
#define IMAGE_HDR_INVALID  2  /* magic missing or length out of range */

/* Validate the image at flash `0x08001800` (slot 1):
 *   1. magic == 0xAA55AA55     → else return IMAGE_HDR_INVALID
 *   2. length  < IMAGE_MAX_SIZE → else return IMAGE_HDR_INVALID
 *   3. CRC32(header_with_crc_and_length_masked + payload) == header.crc
 *      → IMAGE_OK / IMAGE_CRC_MISMATCH
 * Uses the hardware CRC peripheral at `0x40023000` (see `crc.h`). */
int image_verify_crc(void);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_IMAGE_H */
