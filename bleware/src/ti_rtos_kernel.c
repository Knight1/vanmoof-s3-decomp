/* ti_rtos_kernel.c — bleware's small wrappers around TI-RTOS kernel
 * primitives. The actual TI-RTOS APIs live in ROM; this TU owns the
 * thin glue that the application calls.
 *
 * Functions decoded here:
 *
 *   `icall_cs_enter` @ 0x00024DB8 — disable Hwi + Task, pack both
 *                                   restore keys into one 32-bit word.
 *   `icall_cs_exit`  @ 0x000266B2 — restore Task and Hwi from a packed
 *                                   key (reverse order, LIFO).
 *   `bios_abort`     @ 0x0002669C — `while (1) {}` halt loop. Tail-calls
 *                                   into `0x0002773E` after the loop —
 *                                   dead code per Ghidra ("WARNING:
 *                                   Removing unreachable block"), but
 *                                   preserved here for completeness.
 *
 * The four trivial ROM-thunk pass-throughs (`ti_queue_put`,
 * `ti_event_post`, `ti_task_self`, `bios_get_thread_state`) remain as
 * weak no-ops in `hal_stubs.S` — they're literally `ldr.w pc, [literal]`
 * one-instruction tail-calls in the OEM, and won't link to anything
 * useful until we vendor the SimpleLink SDK 3.40.
 */

#include <stdint.h>

#include "bleware.h"

/* Underlying TI-RTOS ROM thunks (each is `ldr.w pc, [literal]` in the
 * OEM, located at flash 0x00027B20..0x00027B30). */
extern uint32_t bios_hwi_disable (void);                  /* @ 0x1002EA24 */
extern uint32_t bios_task_disable(void);                  /* @ 0x1002EB54 */
extern void     bios_task_restore(uint32_t task_key);     /* @ 0x1002EC46 */
extern void     bios_hwi_restore (uint32_t hwi_key);      /* @ 0x1002DB60 */

/* Trampoline reached after `bios_abort`'s halt loop. Per Ghidra it's
 * unreachable, but the OEM keeps the bl in flash. */
extern void abort_continuation(void);                     /* @ 0x0002773E */

uint32_t icall_cs_enter(void)
{
    uint32_t hwi_key  = bios_hwi_disable();   /* OEM call order: thunk
                                               * 0x1002EA24 first... */
    uint32_t task_key = bios_task_disable();  /* ...then thunk 0x1002EB54 */

    /* Pack low half = first key, high half = second key. The OEM uses
     * a `bfi` instruction; this expression compiles to the same. */
    return (hwi_key & 0xFFFFu) | ((task_key & 0xFFFFu) << 16);
}

void icall_cs_exit(uint32_t key)
{
    bios_task_restore(key >> 16);
    bios_hwi_restore (key & 0xFFFFu);
}

void bios_abort(void)
{
    /* The OEM uses a stack-resident flag (initialised to 1) as the
     * loop condition — match it so a JTAG probe attached at runtime
     * can clear the flag and step past the halt. */
    volatile uint8_t halted = 1;
    while (halted != 0) {
        /* spin */
    }
    abort_continuation();
}
