#ifndef MAINWARE_SCHEDULER_H
#define MAINWARE_SCHEDULER_H

#include <stdint.h>

/* Muco-runtime one-shot scheduler API (the mainware variant — 48 slots vs
 * mainboot's 16). The slot table lives at SRAM 0x200004C0; each slot is
 * identified by an 8-bit id 0..47, with the sentinel 0xFA meaning "no slot
 * held".
 *
 * Implementation lives at:
 *   scheduler_tick           0x080306D8  — called from SysTick_Handler
 *   scheduler_alloc          0x0803073C
 *   scheduler_release        0x080307A8
 *   scheduler_start          0x08030800
 *   scheduler_slot_is_idle   0x08030838
 *
 * A slot moves through two independent states, each its own bitmap in the
 * table (see `struct scheduler`):
 *   - allocated: reserved by a caller (scheduler_alloc → scheduler_release).
 *   - armed:     has a live countdown (scheduler_start → expiry / release).
 * scheduler_tick only scans the `armed` bitmap; scheduler_release clears both.
 */

#define SCHED_SLOT_NONE    0xFAu   /* slot id sentinel: "no slot held" */
#define SCHED_SLOTS        48u     /* slot ids 0..47 */
#define SCHED_BITMAP_BYTES 6u      /* SCHED_SLOTS / 8 */

typedef void (*sched_cb_t)(void);

/* The scheduler slot table (SRAM 0x200004C0, 0x190 bytes). The two 6-byte
 * bitmaps are addressed `bitmap[slot >> 3] & (1 << (slot & 7))`; the two
 * trailing pad bytes after each align the next array to a 4-byte boundary,
 * matching the OEM's +0x08 / +0x10 / +0xD0 field offsets. */
struct scheduler {
    uint8_t    allocated[SCHED_BITMAP_BYTES]; /* +0x00 — slot-allocated bitmap */
    uint8_t    _pad0[2];                      /* +0x06 */
    uint8_t    armed[SCHED_BITMAP_BYTES];     /* +0x08 — slot-armed bitmap */
    uint8_t    _pad1[2];                      /* +0x0E */
    sched_cb_t callbacks[SCHED_SLOTS];        /* +0x10 — per-slot one-shot callback */
    uint32_t   counters[SCHED_SLOTS];         /* +0xD0 — per-slot down-counter (ticks) */
};
_Static_assert(__builtin_offsetof(struct scheduler, armed)     == 0x08, "");
_Static_assert(__builtin_offsetof(struct scheduler, callbacks) == 0x10, "");
_Static_assert(__builtin_offsetof(struct scheduler, counters)  == 0xD0, "");
_Static_assert(sizeof(struct scheduler) == 0x190, "");

extern struct scheduler g_scheduler;   /* SRAM 0x200004C0 */

/* Clear both slot bitmaps so all 48 slots start free. Called once during
 * application init (OEM scheduler_init at 0x080306C0); returns 1. */
int scheduler_init(void);

/* SysTick service: for every armed slot, decrement its counter (floored at 0)
 * and, when the counter lands on 1, invoke its callback once. A slot armed
 * with N ticks therefore fires after N-1 SysTick periods; the login lockout
 * uses a NULL callback and just polls scheduler_slot_is_idle(). */
void scheduler_tick(void);

/* Allocate a free slot. Returns slot id 0..47. If every slot is taken the
 * OEM treats it as fatal (muco_assert_fail, "src/time.c":127) and never
 * returns — so a successful return is always a valid id. */
uint8_t scheduler_alloc(void);

/* Release the slot whose id is at *slot_ref. Clears both the armed and the
 * allocated bit, zeroes counter+callback, then rewrites *slot_ref =
 * SCHED_SLOT_NONE. Returns 1 if the slot was valid, 0 if *slot_ref was
 * already out-of-range. */
int scheduler_release(uint8_t *slot_ref);

/* Arm an existing slot with `ticks` SysTick periods and `cb` callback.
 * Returns 1 on success, 0 if slot is out of range. Re-calling on an
 * already-armed slot just resets the counter (used by login_handler to
 * extend its lockout window on every input during cooldown). */
int scheduler_start(uint8_t slot, uint32_t ticks, sched_cb_t cb);

/* Return 1 iff slot is in range AND its counter is currently zero (expired or
 * never armed). Returns 0 for out-of-range ids, including SCHED_SLOT_NONE — so
 * callers must check the byte isn't the sentinel before trusting a 0 here. */
int scheduler_slot_is_idle(uint8_t slot);

/* Timer/task name-register hook — a no-op `bx lr` stub in this release build
 * (OEM scheduler_set_timer_name, 0x08029B70). Callers pair it with scheduler_start. */
void scheduler_set_timer_name(uint8_t slot, uint32_t ticks, const char *name);

#endif
