#include <stdint.h>

#include "util.h"

/* Packed-BCD <-> binary byte converters. The OEM uses these for the RTC /
 * clock fields; both are pure and were transcribed directly from the OEM
 * arithmetic (0x0802311C / 0x08022F2E). */

uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0Fu) + (bcd >> 4) * 10u);
}

uint8_t bin_to_bcd(uint8_t bin)
{
    uint8_t tens = 0;

    while (bin > 9u) {
        bin = (uint8_t)(bin - 10u);
        tens++;
    }
    return (uint8_t)(bin | (uint8_t)(tens << 4));
}
