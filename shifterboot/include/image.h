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

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_IMAGE_H */
