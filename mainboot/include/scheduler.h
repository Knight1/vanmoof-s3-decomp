#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_SLOTS 16

typedef void (*sched_cb_t)(void);

/* Soft-timer / callback dispatch table consulted on every SysTick.
 * OEM lives in .bss at SRAM 0x2000038C. Layout reverse-engineered
 * from FUN_08003840 (scheduler_tick): a 16-bit enable bitmap at
 * +0x04, sixteen callback function pointers at +0x08..+0x47, and
 * sixteen tick counters at +0x48..+0x87. The first 4 bytes are
 * unused by scheduler_tick itself; their purpose (slot count? a
 * lock? a tick-rate divider?) will become visible once the
 * registration API is decoded (likely FUN_080006a0 / FUN_080006c8).
 */
struct scheduler {
    uint32_t   _reserved0;                /* +0x00 — not touched by scheduler_tick */
    uint16_t   enabled_mask;              /* +0x04 — one bit per slot, bit i = slot i */
    uint16_t   _reserved6;                /* +0x06 — padding to 32-bit align callbacks */
    sched_cb_t callbacks[SCHED_SLOTS];    /* +0x08..+0x47 — one entry per slot */
    uint32_t   counters[SCHED_SLOTS];     /* +0x48..+0x87 — one entry per slot */
};

extern struct scheduler g_scheduler;

/* SysTick poll: for each enabled slot, decrement its counter (if
 * non-zero) and invoke its callback exactly when the counter
 * transitions to 1. The 2-tick window then settles at 0 on the next
 * pass with no further callback until the counter is re-armed by
 * the registration API. */
void scheduler_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
