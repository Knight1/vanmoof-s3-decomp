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
