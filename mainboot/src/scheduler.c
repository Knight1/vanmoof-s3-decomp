#include <stdint.h>

#include "scheduler.h"

struct scheduler g_scheduler;

void scheduler_tick(void)
{
    const uint8_t *mask_bytes = (const uint8_t *)&g_scheduler.enabled_mask;

    /* OEM holds the slot index in a uint8_t register (uxtb after each
     * increment); SCHED_SLOTS == 16 so the wrap-to-byte has no
     * observable effect — kept implicit for readability. */
    for (uint32_t i = 0; i < SCHED_SLOTS; i++) {
        if (((mask_bytes[i >> 3] >> (i & 7u)) & 1u) == 0u) {
            continue;
        }

        uint32_t c = g_scheduler.counters[i];
        if (c != 0u) {
            c -= 1u;
            g_scheduler.counters[i] = c;
        }

        if (c == 1u) {
            sched_cb_t cb = g_scheduler.callbacks[i];
            if (cb != (sched_cb_t)0) {
                cb();
            }
        }
    }
}
