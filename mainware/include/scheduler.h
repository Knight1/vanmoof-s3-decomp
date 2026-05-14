#ifndef MAINWARE_SCHEDULER_H
#define MAINWARE_SCHEDULER_H

#include <stdint.h>

/* Muco-runtime one-shot scheduler API (the mainware variant — 48 slots
 * vs mainboot's 16). The slot table lives at SRAM 0x200004C0; each
 * slot is identified by an 8-bit id 0..47, with the sentinel 0xFA
 * meaning "no slot held".
 *
 * Implementation lives at:
 *   scheduler_tick           0x080306D8  — called from SysTick_Handler
 *   scheduler_alloc          0x0803073C
 *   scheduler_release        0x080307A8
 *   scheduler_start          0x08030800
 *   scheduler_slot_is_idle   0x08030838
 */

#define SCHED_SLOT_NONE 0xFAu   /* slot id sentinel: "no slot held" */

typedef void (*sched_cb_t)(void);

/* Allocate a free slot. Returns slot id 0..47, or SCHED_SLOT_NONE if
 * all slots are occupied (also logs an error string in that case). */
uint8_t scheduler_alloc(void);

/* Release the slot whose id is at *slot_ref. Clears the enabled bit,
 * zeroes counter+callback, then rewrites *slot_ref = SCHED_SLOT_NONE.
 * Returns 1 if the slot was valid, 0 if *slot_ref was already
 * out-of-range. */
int scheduler_release(uint8_t *slot_ref);

/* Arm an existing slot with `ticks` SysTick periods and `cb` callback.
 * Returns 1 on success, 0 if slot is out of range. Re-calling on an
 * already-armed slot just resets the counter (used by login_handler
 * to extend its lockout window on every input during cooldown). */
int scheduler_start(uint8_t slot, uint32_t ticks, sched_cb_t cb);

/* Return 1 iff slot is in range AND its counter is currently zero
 * (i.e., the slot has either expired or was never armed). Returns 0
 * for out-of-range slot ids (including the SCHED_SLOT_NONE sentinel).
 *
 * In a typical "release-on-expiry" pattern, a caller stores an
 * allocated slot id in some state byte, then later checks
 * `scheduler_slot_is_idle(*state_byte)` to see if the timer fired —
 * but only after first checking that the byte isn't SCHED_SLOT_NONE,
 * since this function reports 0 for the sentinel as well. */
int scheduler_slot_is_idle(uint8_t slot);

#endif
