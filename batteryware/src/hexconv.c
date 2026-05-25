#include "batteryware.h"

/*
 * Convert a 4-bit nibble (0-15) to an uppercase ASCII hex character.
 *   0-9  → '0'-'9'  (add 0x30)
 *   10-15 → 'A'-'F' (add 0x37)
 */
char nibble_to_hex(uint8_t nibble)
{
    if (nibble < 10) {
        return (char)(nibble + 0x30);
    }
    return (char)(nibble + 0x37);
}

/*
 * Convert an ASCII hex character to a 4-bit nibble (0-15).
 *   '0'-'9' → 0-9   (AND with 0x0F)
 *   'A'-'F' → 10-15 (subtract 0x37)
 *   'a'-'f' → 10-15 (NOT handled — caller must uppercase first)
 */
uint8_t hex_to_nibble(char c)
{
    if ((uint8_t)c > '9') {
        return (uint8_t)(c - 0x37);
    }
    return (uint8_t)c & 0x0F;
}

/*
 * Parse a hex string (skipping first char) as a decimal integer.
 *
 * Reads 'digits' hex characters starting at str[1], converts each
 * via hex_to_nibble, and accumulates the result as decimal:
 *   result = result * 10 + digit
 * (Note: * 10 = (result << 2 + result) << 1 = result * 10)
 */
uint32_t atoi_hex_offset1(char *str, uint8_t digits)
{
    uint32_t result = 0;

    /* First digit at str[0] — used as initial value, no base shift */
    result = hex_to_nibble(str[0]);
    str++;

    for (uint8_t i = 1; i < digits; i++) {
        /* result = result * 10 */
        if (result != 0) {
            result = (result << 2) + result;   /* * 5 */
            result = result << 1;              /* * 10 */
        }
        result += hex_to_nibble(str[0]);
        str++;
    }

    return result;
}
