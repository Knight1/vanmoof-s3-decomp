/* modbus.c — Modbus RTU primitives for shifterboot's OTA server.
 *
 * Shifterboot speaks the same RTU framing as shifterware (3.5-char
 * idle gap, 16-bit CRC suffix, big-endian within byte but the CRC
 * itself transmitted low-byte-first), but exposes only the OTA-flow
 * subset of function codes (cmd 0x81 = apply, cmd 0x82 = stream a
 * 32 B image chunk, etc.). The dispatcher lives in `main`; this file
 * holds the framing primitives those handlers share. */

#include "modbus.h"

#include <stdint.h>

/* RTU CRC parameters (per the Modbus over Serial Line specification). */
#define MODBUS_INIT  0xFFFFu
#define MODBUS_POLY  0xA001u

/* OEM @ 0x08000100 (56 B). Per-byte XOR + 8-bit shift-and-XOR with
 * polynomial 0xA001. Direct return — the result lands in r0, no RAM
 * side effect (the shifterware twin, by contrast, writes its two
 * bytes to globals at 0x200000E7 / 0x200000E8).
 *
 * Implementation notes vs the OEM bytes:
 *   - The OEM emits `asrs` (arithmetic shift right) for the post-XOR
 *     shift instead of `lsrs`. Since the live value never sets bit 31
 *     (CRC is 16-bit, initialised to 0x0000FFFF, only the lower bits
 *     are ever XOR'd), `asrs` is identical to `lsrs` here — a -O0
 *     codegen quirk, not an algorithm choice.
 *   - The outer-loop control is the snapshot-then-decrement idiom:
 *     `tmp = count; count--; if (tmp != 0) iterate`. That is exactly
 *     what gcc emits for `while (count != 0) { ... ; count--; }`
 *     when the test is placed at the loop header. */
uint16_t modbus_crc16(const uint8_t *data, uint16_t count)
{
    uint16_t crc = (uint16_t)MODBUS_INIT;

    while (count != 0u) {
        crc ^= (uint16_t)(*data++);
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ MODBUS_POLY);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
        count--;
    }

    return crc;
}
