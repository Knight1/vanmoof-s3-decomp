#ifndef SHIFTER_WATCHDOG_H
#define SHIFTER_WATCHDOG_H

#include <stdint.h>

void watchdog_init(uint32_t timeout_ms);
void watchdog_kick(void);

#endif /* SHIFTER_WATCHDOG_H */
