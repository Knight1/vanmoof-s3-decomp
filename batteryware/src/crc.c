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

/* CRC-8 polynomial used for flash/firmware verification */
#define CRC8_POLY   0x07

/*
 * CRC-8 calculation.
 *
 * Polynomial 0x07, initial value 0xFF. Processes 'len' bytes.
 * Used for firmware page verification in the flash programming path.
 */
uint8_t crc8_calc(uint8_t *data, int8_t len)
{
    uint8_t crc = 0xFF;

    while (len-- != 0) {
        crc ^= *data++;

        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (uint8_t)(crc << 1) ^ CRC8_POLY;
            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}
