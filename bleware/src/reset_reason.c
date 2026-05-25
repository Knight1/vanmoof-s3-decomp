/* reset_reason.c — returns a human-readable string for the last
 * reset cause (read from the CC2642R1F AON_PMCTL / RESETCTL register
 * via the ROM SysCtrlResetSourceGet API).
 *
 * OEM @ 0x000145AC (58 B).
 */

#include <stdint.h>

const char *reset_reason_string(void)
{
    extern uint32_t bios_get_reset_source(void);

    uint32_t src = bios_get_reset_source();
    switch (src) {
    case 0:  return "power-on";
    case 1:  return "pin-reset";
    case 2:  return "VDDS loss";
    case 4:  return "VDDR loss";
    case 5:  return "clock loss";
    case 6:  return "system reset";
    case 7:  return "warm WDT reset";
    case 8:  return "wake-up from shutdown";
    case 9:  return "wake-up from TCK noise";
    default: return "unknown";
    }
}
