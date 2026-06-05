#include <stdint.h>

#include "crc.h"

/* Modbus RTU CRC-16, reflected poly 0xA001 (the project-wide inter-module-bus
 * CRC). The OEM leaves the running CRC in r0 for both routines; Ghidra typed
 * them void but the callers consume the return. */

uint16_t crc16_modbus_update(uint16_t crc, uint8_t data)
{
    uint32_t v = (uint32_t)crc ^ (uint32_t)data;   /* XOR affects the low byte */

    for (int i = 0; i < 8; i++) {
        if (v & 1u) {
            v = (v >> 1) ^ 0xA001u;
        } else {
            v = v >> 1;
        }
    }
    return (uint16_t)v;
}

uint16_t crc16(const uint8_t *buf, int len, uint16_t crc)
{
    while (len != 0) {
        crc = crc16_modbus_update(crc, *buf);
        len--;
        buf++;
    }
    return crc;
}
