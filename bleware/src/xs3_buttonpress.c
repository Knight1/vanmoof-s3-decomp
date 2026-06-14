/* xs3_buttonpress.c — button-press / bike-lockstate handling.
 *
 * OEM source path string embedded at flash 0x0001E878:
 *   "source/xs3_buttonpress.c"
 *
 * Functions decoded:
 *
 *   `buttonpress_assert_invalid_lockstate`  @ 0x0001E858
 *   `buttonpress_find_conn_slot`            @ 0x0001E918
 *
 * The OEM function-name string passed to monitor_log by the first
 * function is "button_platform_timeout_handler" (flash 0x0002B491), which
 * is the name of the larger OEM routine these helpers were inlined out
 * of; the two bodies here are the only code in this TU that references the
 * buttonpress path string.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware variadic logger (src/monitor_log.c, OEM @ 0x00006D90).
 * Not in bleware.h — declared extern at every call site, as in
 * state_machine.c. */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* TI-RTOS Semaphore module ROM thunks (same handles used by
 * src/ble_connection.c). pend blocks forever with timeout 0xFFFFFFFF. */
extern int  ti_semaphore_pend(void *sem, uint32_t timeout_ticks);  /* FUN_00027DF0 -> ROM 0x1002BFB0 */
extern void ti_semaphore_post(void *sem);                          /* FUN_00027DD8 -> ROM 0x1002CD20 */

/* Log-enable gate byte. RAM 0x20005670 (literal pool entry at flash
 * 0x0001E8B0). Non-zero => the assert path emits its monitor_log line. */
extern volatile uint8_t g_buttonpress_log_enabled;   /* *0x20005670 */

/* Per-connection state table shared with src/ble_connection.c. Stride
 * 0x7C, 3 slots. Only the fields this TU touches are modelled; the rest
 * of the layout lives in ble_connection.c's struct.
 *
 * The OEM loads the table base as a literal-pool immediate (flash
 * 0x0001E974 holds 0x20004158), not via a named pointer global, so this
 * TU resolves it through a fixed RAM address rather than redeclaring the
 * `g_ble_connection_table` symbol (which ble_connection.c models with an
 * incompatible pointer-to-const type). */
struct buttonpress_conn_slot {
    uint8_t   pad00[0x24];        /* +0x00..0x23 */
    void     *sem;                /* +0x24 — per-slot Semaphore handle */
    uint8_t   pad28[0x20];        /* +0x28..0x47 */
    uint16_t  conn_handle;        /* +0x48 — set when slot is allocated */
    uint8_t   pad4a[0x0A];        /* +0x4A..0x53 */
    uint16_t  match_tag;          /* +0x54 — compared against `tag` arg */
    uint8_t   pad56[0x26];        /* +0x56..0x7B */
};

#define BLE_CONNECTION_TABLE_BASE  0x20004158u   /* flash literal @ 0x0001E974 */

#define SEM_TIMEOUT_FOREVER       0xFFFFFFFFu
#define BUTTONPRESS_CONN_SLOTS    3

/* Assert/log wrapper for an invalid bike-lockstate. When the log-enable
 * gate byte at RAM 0x20005670 is set, emit a "Invalid bike-lockstate
 * (%d)" line tagged with this TU's path / line 184 / the OEM source
 * function name.
 *
 * OEM quirk (preserved): the `%d` argument is the gate byte's own value
 * — the same byte just loaded for the enable test — not a separate
 * lockstate parameter. The OEM body takes no arguments; the value
 * formatted is whatever non-zero value the gate byte holds.
 *
 * OEM @ 0x0001E858 (30 B). */
void buttonpress_assert_invalid_lockstate(void)
{
    uint8_t gate = g_buttonpress_log_enabled;
    if (gate != 0) {
        monitor_log("source/xs3_buttonpress.c", 184,
                    "button_platform_timeout_handler", 1,
                    "Invalid bike-lockstate (%d)", gate);
    }
}

/* Scan the 3 BLE-connection slots for the one whose `conn_handle`
 * (+0x48) equals its own slot index AND whose `match_tag` (+0x54)
 * equals `tag`. On a hit, write the matching slot index into
 * `*out_index` and return 0; if no slot matches, return -1.
 *
 * Each slot is examined under its per-slot semaphore (pend on +0x24
 * before the field reads, post after), matching the locking template
 * used by the other ble_connection accessors. The scan stops at the
 * first match (the loop's top-of-iteration test bails once the result
 * has been set to 0).
 *
 * OEM @ 0x0001E918 (88 B). This body is an undefined (un-carved)
 * function in Ghidra — it is reached only through a register-indirect
 * call, so it carries no FUN_ symbol and has no static xref.
 *
 * OEM quirk (preserved): the slot index is compared against the stored
 * conn_handle (`index == slot->conn_handle`), mirroring the
 * `conn == e->conn_handle` sanity check the sibling accessors run with
 * the connection handle — here the loop index plays the role of the
 * handle. The matched index is stored as a 16-bit value. */
int buttonpress_find_conn_slot(uint32_t tag, uint16_t *out_index)
{
    struct buttonpress_conn_slot *table =
        (struct buttonpress_conn_slot *)(uintptr_t)BLE_CONNECTION_TABLE_BASE;
    int result = -1;

    for (int index = 0; index < BUTTONPRESS_CONN_SLOTS; index++) {
        if (result >= 0) {
            break;
        }

        struct buttonpress_conn_slot *slot = &table[index];

        ti_semaphore_pend(slot->sem, SEM_TIMEOUT_FOREVER);
        if ((uint16_t)index == slot->conn_handle && tag == slot->match_tag) {
            *out_index = (uint16_t)index;
            result = 0;
        }
        ti_semaphore_post(slot->sem);
    }

    return result;
}
