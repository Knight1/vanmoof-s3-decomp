#ifndef MAINWARE_UTIL_H
#define MAINWARE_UTIL_H

#include <stdint.h>

/* Packed-BCD <-> binary byte converters (RTC / time-field helpers). */

/* Decode one packed-BCD byte (high nibble = tens, low nibble = units) to its
 * binary value 0..99+. OEM bcd_to_bin at 0x0802311C. */
uint8_t bcd_to_bin(uint8_t bcd);

/* Encode a binary byte (0..99) to packed BCD. OEM bin_to_bcd at 0x08022F2E. */
uint8_t bin_to_bcd(uint8_t bin);

#endif
