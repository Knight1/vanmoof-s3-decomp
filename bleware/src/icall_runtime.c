/* icall_runtime.c — TI BLE-stack ICall service-entity helpers.
 *
 * Owns the small set of leaf functions that walk the ICall entity
 * registry — a fixed array of (up-to-6) records that map BIOS task
 * handles to ICall entity ids. Each entity id is what the rest of the
 * stack uses to route service messages to / from a task.
 *
 * The registry lives at RAM `g_icall_entities` (= flash DAT_00020C9C →
 * 0x20004E78). It is populated during stack init (not yet decoded)
 * and read-only after that.
 */

#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* ---- ROM thunks (TI-RTOS / BIOS) -------------------------------- */
extern void    *ti_task_self(void);            /* thunk_EXT_FUN_1002EAF4 */
extern uint32_t bios_get_thread_state(void);   /* thunk_EXT_FUN_1002EA94 */

/* ---- Critical-section pair (ICall internal lock) ---------------- */
/* Pack of `Hwi_disable()` (low 16) and `Task_disable()` (high 16). */
extern uint32_t icall_cs_enter(void);          /* FUN_00024DB8 */
extern void     icall_cs_exit(uint32_t key);   /* FUN_000266B2 */

/* ---- ICall entity registry --------------------------------------
 * Each record is 12 bytes:
 *   +0x0 u16   in_use       0 = end-of-table sentinel
 *   +0x2 u16   pad
 *   +0x4 void**task_holder  *task_holder == registered Task handle
 *   +0x8 u32   service_msg_fxn_or_state (not read here)
 * The table has up to 6 entries; iteration stops at the first slot
 * with `in_use == 0`. */
struct icall_entity {
    uint16_t  in_use;
    uint16_t  pad;
    void    **task_holder;
    uint32_t  reserved;
};

extern struct icall_entity *const g_icall_entities;   /* DAT_00020C9C */

#define ICALL_ENTITY_INVALID  0xffu
#define ICALL_ENTITY_MAX      6

int icall_caller_entity(void)
{
    void    *self;
    uint32_t cs_key;
    unsigned i;
    unsigned result = ICALL_ENTITY_INVALID;

    self = ti_task_self();

    /* BIOS thread-state values 0 and 1 mean we're not in a runnable
     * task context — return invalid without touching the registry. */
    if ((bios_get_thread_state() & ~1u) == 0u) {
        return ICALL_ENTITY_INVALID;
    }

    cs_key = icall_cs_enter();

    for (i = 0; i < ICALL_ENTITY_MAX; i++) {
        if (g_icall_entities[i].in_use == 0u) {
            break;
        }
        if (*g_icall_entities[i].task_holder == self) {
            result = (uint8_t)i;
            break;
        }
    }

    icall_cs_exit(cs_key);
    return (int)result;
}
