#ifndef SHIFTERBOOT_CRC_H
#define SHIFTERBOOT_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MM32F031 hardware CRC peripheral at 0x40023000. The peripheral
 * implements the standard STM32/MM32 polynomial (0x4C11DB7, MPEG-2),
 * init `0xFFFFFFFF`, no input/output reflection — confirmed by
 * shifterware's image-CRC patcher (`shifterware/tools/patch_image_crc.py`)
 * yielding bit-identical results against this engine. */
#define CRC_DR_ADDR   (0x40023000u)

/* Reset the CRC engine to its initial value (`0xFFFFFFFF`). The
 * OEM helper lives in the MindMotion HAL at `0x080013AC` (8 B,
 * vendor-stock per docs/progress.md). Declared `extern` here; its
 * body will arrive once the MindMotion BSP is vendored in. */
extern void CRC_ResetDR(void);

/* Feed `count` words from `src` into the CRC engine, return the
 * resulting CRC value. The OEM helper @ `0x080013CC` (32 B) — a
 * bare write-loop into `*(uint32_t *)0x40023000` (= CRC->DR) with
 * a final read to fetch the accumulator. */
uint32_t crc32_words(const uint32_t *src, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_CRC_H */
