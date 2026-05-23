/* log_emit.c — variadic log-emit helper.
 *
 * OEM symbol: `FUN_0001AC6C` @ 0x0001AC6C (renamed in Ghidra to
 * `log_emit_v`). All over the bleware codebase you see calls of the
 * shape `log_emit_v(0x10, "fmt-string", ...)` — this is the bleware
 * application's structured-log primitive, distinct from:
 *
 *   - `monitor_log(file, line, fn, level, fmt, ...)` (FUN_00006D90) —
 *     the location-aware console log used by the monitor module
 *   - `log_submit(channel, block, len)` — raw block submit to the
 *     external-flash circular log (used by cmd_log_inject)
 *
 * Mechanism: a small "service message" envelope is built on the stack
 * containing a type byte, an originator id, the va-list pointer, and
 * the format-string pointer. The envelope is dispatched via the TI
 * BLE-stack ICall facility (`icall_send_service_msg`) to the logger
 * service (id 0x10 in observed call sites). The caller then blocks for
 * up to 1000 ms waiting for an ack from the logger; on success the
 * reply message is freed and the helper returns. A timeout or
 * dispatch failure is treated as fatal and abort()s.
 *
 * Return value: the logger writes a status code into the first-
 * variadic-argument slot before acking, so `*ap` is the result. Most
 * call sites discard the return; the GAP-advertising path
 * (FUN_00016b04) uses it to fault-log via `panic(rc, line, file)` when
 * non-zero.
 *
 * Stack envelope layout produced by the OEM (FUN_0001E618 prepends an
 * 8-byte ICall header just before this struct):
 *
 *   +0x00 u16  type     0x0014   ("varargs format-string log")
 *   +0x02 u8   origin   0xff     (anonymous / local task)
 *   +0x03 u8   pad
 *   +0x04 u32  args_ap  &param_3 (pointer to first variadic, also
 *                                 written-back with status on reply)
 *   +0x08 u32  fmt      param_2
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* ---- ICall helpers (TI BLE-stack inter-task message facility) ---- */
extern int  icall_caller_entity(void);                              /* FUN_00020c54 */
extern void icall_send_service_msg(int from_entity, int service_id,
                                   int msg_kind, void *envelope);   /* FUN_00025100 */
extern int  icall_wait_match(int timeout_ms,
                             int (*pred)(uint16_t src, uint8_t kind,
                                         void *msg),
                             uint16_t *out_src, uint8_t *out_kind,
                             void **out_msg);                       /* FUN_0001134c */
extern void icall_free_message(void *msg);                          /* FUN_000275fa */

/* BIOS scheduler state — running threads return a value with bit 1 set;
 * states 0 and 1 mean the ICall queue isn't usable and we must abort. */
extern uint32_t bios_get_thread_state(void);   /* thunk_EXT_FUN_1002ea94 */
extern void     bios_abort(void) __attribute__((noreturn));  /* FUN_0002669c */

/* Logger-service reply-match predicate (file-static fn pointer at
 * `DAT_0001ACE8` → code at 0x000273F4). Opaque from this TU. */
extern int log_reply_match_pred(uint16_t src, uint8_t kind, void *msg);

uint32_t log_emit_v(uint32_t service_id, const char *fmt, ...)
{
    struct {
        uint16_t  type;
        uint8_t   origin;
        uint8_t   pad;
        uint32_t *args_ap;
        const char *fmt;
    } envelope;
    va_list ap;
    void   *reply = NULL;
    int     rc;

    va_start(ap, fmt);

    /* Refuse to dispatch from contexts where ICall isn't valid. */
    if ((bios_get_thread_state() & ~1u) == 0u) {
        bios_abort();
    }

    /* AAPCS va_list is `struct __va_list { void *__ap; }` — `__ap`
     * already points at the first variadic argument's stack slot. The
     * OEM dereferences this pointer twice: once to walk varargs while
     * formatting, and once on return to read the status word that the
     * logger wrote back into the same slot. */
    envelope.type    = 0x0014u;
    envelope.origin  = 0xffu;
    envelope.pad     = 0u;
    envelope.args_ap = (uint32_t *)ap.__ap;
    envelope.fmt     = fmt;

    icall_send_service_msg(icall_caller_entity(),
                           (int)service_id,
                           3,                       /* msg_kind */
                           &envelope.args_ap);

    rc = icall_wait_match(1000, log_reply_match_pred,
                          NULL, NULL, &reply);
    if (rc != 0) {
        bios_abort();
    } else if (reply != NULL) {
        icall_free_message(reply);
    }

    return *envelope.args_ap;
}
