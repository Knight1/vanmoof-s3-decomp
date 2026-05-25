#include "batteryware.h"

/* CRC-16 polynomial used by VanMoof (Modbus CRC-16) */
#define CRC16_POLY  0xA001

/*
 * Modbus CRC-16 calculation.
 *
 * Standard Modbus CRC-16: init = 0xFFFF, polynomial 0xA001.
 * Processes 'len' bytes from 'data' and returns the 16-bit CRC.
 * Used for command frame validation in the Modbus protocol handler.
 */
uint16_t crc16_calc(uint8_t *data, int16_t len)
{
    uint16_t crc = 0xFFFF;

    while (len-- != 0) {
        crc ^= *data++;

        for (uint8_t i = 0; i < 8; i++) {
            if ((crc & 1) != 0) {
                crc = (crc >> 1) ^ CRC16_POLY;
            } else {
                crc = crc >> 1;
            }
        }
    }

    return crc;
}
