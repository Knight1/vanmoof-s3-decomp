#ifndef MAINWARE_STATES_H
#define MAINWARE_STATES_H

#include <stdint.h>

/* Map an alarm/bike state code (0..0x3D) to a human-readable name string, used
 * for logging (OEM alarm_state_name, 0x08032DF0). Returns "UNKNOWN" for an
 * out-of-range code. */
const char *alarm_state_name(uint32_t state);

#endif
