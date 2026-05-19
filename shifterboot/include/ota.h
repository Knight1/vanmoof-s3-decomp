#ifndef SHIFTERBOOT_OTA_H
#define SHIFTERBOOT_OTA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-chunk OTA flash writer. Called from `main`'s cmd-0x82
 * Modbus OTA payload-streaming loop (`0x080004B6`).
 *
 * Walks `count_bytes` of the inbound frame's payload — starting at
 * `frame + 11` (skipping the 11-byte Modbus header: slave, fcode,
 * length, sub-id, address bytes, etc.) — packs each consecutive
 * byte pair into a little-endian halfword (`lo | (hi << 8)`),
 * stores the halfwords into the SRAM scratch buffer at
 * `0x200000F2`, then flushes 16 halfwords (32 B) to flash at
 * `dst` via `flash_program_range`.
 *
 * Note: the flash-flush count is **fixed at 16 halfwords**
 * regardless of `count_bytes`. The OEM caller passes 32 for
 * normal chunks; for the last (partial) chunk it may pass less,
 * and the function flushes whatever is in the scratch buffer up
 * to the 16-halfword slot — including stale halfwords from a
 * prior call if `count_bytes < 32`. Mirroring this behaviour
 * matches OEM bytes. */
void ota_program_chunk(uint32_t dst,
                       const uint8_t *frame,
                       uint8_t count_bytes);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_OTA_H */
