/* crc32.c — CRC-32/zlib (reflected, polynomial 0xEDB88320).
 *
 * The OEM computes CRC-32 on-the-fly per byte — no lookup table.
 * `crc32_step_byte` runs 8 iterations of the shift-XOR loop against
 * `DAT_00026548 = 0xEDB88320`. `crc32_le` wraps it in a byte-by-byte
 * loop over the input buffer. No final XOR — callers that want the
 * standard zlib CRC pass seed=0xFFFFFFFF and XOR the result with
 * 0xFFFFFFFF themselves (the secrets store stores the raw non-XOR'd
 * value, so most callers forego the final XOR).
 */

#include <stdint.h>

#include "bleware.h"

/* CRC-32 polynomial — OEM DAT_00026548 = 0xEDB88320 (reflected). */
#define CRC32_POLY  0xEDB88320u

/* Process one byte through the reflected CRC-32 shift register.
 * OEM at 0x00026534 (22 B). */
static uint32_t crc32_step_byte(uint32_t byte_val)
{
    uint32_t v = byte_val & 0xFFu;
    int i = 8;
    do {
        if (v & 1u) {
            v = CRC32_POLY ^ (v >> 1);
        } else {
            v >>= 1;
        }
        i--;
    } while (i != 0);
    return v;
}

/* Compute the reflected CRC-32 over `len` bytes starting at `buf`,
 * with the given `seed`. See the block comment above for the final-XOR
 * convention. OEM @ 0x00025198 (58 B). */
uint32_t crc32_le(uint32_t seed, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       crc = seed;

    while (len != 0) {
        uint32_t step = crc32_step_byte((*p ^ crc) & 0xFFu);
        crc = step ^ (crc >> 8);
        p++;
        len--;
    }
    return crc;
}

/* CRC-16/Modbus — polynomial 0xA001 (reflected 0x8005), initial
 * value 0xFFFF. Used to validate the backoffice GATT message payload
 * in provisioning.c. OEM @ 0x0002651C (48 B). */
uint32_t crc16_modbus(const uint8_t *buf, int len, uint32_t seed)
{
    uint32_t crc = seed & 0xFFFFu;

    while (len != 0) {
        uint8_t byte_val = *buf;
        crc ^= byte_val;
        for (int i = 0; i < 8; i++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xA001u;
            } else {
                crc >>= 1;
            }
        }
        buf++;
        len--;
    }
    return crc;
}
