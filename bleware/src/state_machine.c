/* state_machine.c — bleware state-machine notify primitive + dispatcher.
 *
 * Functions decoded:
 *
 *   `state_machine_post`           @ 0x00017C6C
 *   `sm_run_dispatcher`            @ 0x00019C30
 *   `sm_dispatch_event_callback`   @ 0x0002719C
 *
 * Wraps the per-event payload in a tiny `{state_id : u8, _pad : u8,
 * len : u16, payload[len]}` envelope, hands it to the queue-publish
 * helper at FUN_000219A2 with `kind = 0x32` (the state-machine event
 * kind), then frees the envelope once the queue copies it.
 *
 * The OEM build's monitor_log line cites `source/xs3_statemachine.c`
 * line 388; the allocation-failure branch logs at log level 2 and
 * tail-calls into the firmware abort routine (FUN_0001F7F8). Both the
 * abort and the queue helper are still weak-stubbed.
 *
 * `state_machine_post` is the publish-side counterpart to the BLE
 * stack's state-machine consumer (kind 0x32 in the dispatcher inside
 * bluetoothtask_main). Used by `backoffice_auth_session_init` to notify
 * the state machine of an auth-state transition (event_id = 0x18).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

extern void *memcpy(void *dst, const void *src, unsigned int n);

extern void  monitor_log(const char *file, int line, const char *fn,
                         int level, const char *fmt, ...);
extern void  firmware_abort(void) __attribute__((noreturn));   /* FUN_0001F7F8 */

/* Publish a `{kind, payload_ptr, payload_len, tag}` envelope to the
 * bluetoothtask user-message queue (allocates its own copy of the
 * payload, free responsibility transfers to the consumer). Returns 0
 * on success, 1 if the inner alloc or queue_put failed. */
extern int   task_queue_publish_envelope(uint32_t kind, const void *payload,
                                         uint16_t len, uint32_t tag);
                                                              /* FUN_000219A2 */

#define STATE_MACHINE_QUEUE_KIND   0x32u
#define STATE_MACHINE_LOG_TAG      0xE48F2A39u   /* DAT_00017D0C — opaque */
#define STATE_MACHINE_LOG_FN       "post_event"  /* DAT_00017D08 → "post_event" */
#define STATE_MACHINE_SRC_FILE     "source/xs3_statemachine.c"
#define STATE_MACHINE_LOG_LINE     0x184         /* line 388 in the OEM source */

void state_machine_post(uint32_t state_id, const void *payload, uint16_t len)
{
    uint16_t total = (uint16_t)(len + 4u);
    uint8_t *buf   = monitor_alloc(total);
    if (buf == NULL) {
        monitor_log(STATE_MACHINE_SRC_FILE, STATE_MACHINE_LOG_LINE,
                    STATE_MACHINE_LOG_FN, 2,
                    "Could not allocate memory for X", state_id);
        firmware_abort();
    }

    buf[0] = (uint8_t)state_id;
    /* buf[1] is implicit padding. */
    buf[2] = (uint8_t)(len     );
    buf[3] = (uint8_t)(len >> 8);
    memcpy(buf + 4, payload, len);

    task_queue_publish_envelope(STATE_MACHINE_QUEUE_KIND, buf, total,
                                STATE_MACHINE_LOG_TAG);
    monitor_free(buf);
}

/* ---------------------------------------------------------------------
 * State-machine event dispatcher (consume side)
 * ---------------------------------------------------------------------
 *
 * The state machine is a flat, table-driven design. A "state table"
 * (g_sm_state_table @ flash 0x0002B228) is indexed by the current-state
 * byte and yields a {transitions, count} record per state; each
 * transition record holds an event id, a handler, and two candidate
 * next-state bytes (one for the HANDLED outcome, one for everything
 * else). The live current-state byte sits at g_sm_context + 1 (RAM
 * 0x20005951).
 *
 * Per-state record (8 bytes):
 *     +0x00  const sm_transition_t *transitions
 *     +0x04  uint32_t               count   (read as a byte by the OEM)
 *
 * Per-transition record (8 bytes):
 *     +0x00  uint8_t  event_id
 *     +0x01  uint8_t  next_state_handled   (state to enter when the
 *                                           handler returns HANDLED)
 *     +0x02  uint8_t  next_state_default   (state to enter otherwise)
 *     +0x03  uint8_t  _pad
 *     +0x04  sm_handler_t handler
 *
 * The handler signature is (event_id, payload, len) and the HANDLED
 * sentinel is the 32-bit value 0x03000000. The sentinel value 0x02 in
 * either next-state byte means "do not change state".
 */

#define SM_HANDLED            0x03000000u   /* handler "consumed it" sentinel */
#define SM_STATE_NO_CHANGE    0x02u         /* next-state byte == 2 -> stay   */

struct sm_transition {
    uint8_t  event_id;             /* +0x00 */
    uint8_t  next_state_handled;   /* +0x01 */
    uint8_t  next_state_default;   /* +0x02 */
    uint8_t  _pad3;                /* +0x03 */
    uint32_t (*handler)(uint32_t event_id, const void *payload, uint32_t len); /* +0x04 */
};

struct sm_state {
    const struct sm_transition *transitions;  /* +0x00 */
    uint32_t                    count;        /* +0x04 (OEM reads the low byte) */
};

/* Flash-resident state table (DAT_00019CB4 -> 0x0002B228) and the RAM
 * state-machine context block (DAT_00019CB8 -> 0x20005950); the live
 * current-state byte is at context[1]. */
extern const struct sm_state g_sm_state_table[];   /* 0x0002B228 */
extern volatile uint8_t       g_sm_context[];       /* 0x20005950, [1] = cur state */

/* Run one event through the current state's transition list.
 *
 * Linear-scans the current state's transitions for an entry whose
 * handler is non-NULL and whose event_id equals `event_id`. On a match,
 * invokes the handler and updates the current-state byte from the
 * matched record:
 *
 *   - handler returned HANDLED  -> next_state = record.next_state_handled
 *   - otherwise                 -> next_state = record.next_state_default
 *
 * In both cases a next-state value of 2 (SM_STATE_NO_CHANGE) leaves the
 * current state untouched.
 *
 * OEM quirk (preserved): after the handler runs, the OEM re-reads the
 * current-state byte and recomputes the transitions pointer from *that*
 * (possibly handler-mutated) state, then indexes it by the *original*
 * matched-entry offset to fetch the next-state byte. So if a handler
 * mutates the state byte itself, the next-state field is taken from the
 * NEW state's transition list at the old matched index. Reproduced
 * verbatim — do not "simplify" by caching the matched record pointer.
 *
 * Also note the OEM reads `record.next_state_handled` (offset +1) on the
 * HANDLED branch but `record.next_state_default` (offset +2) on the
 * default branch — two distinct byte fields.
 *
 * OEM @ 0x00019C30. */
void sm_run_dispatcher(uint32_t event_id, const void *payload, uint32_t len)
{
    const struct sm_state *table = g_sm_state_table;
    volatile uint8_t      *ctx   = g_sm_context;

    uint8_t cur_state = ctx[1];
    const struct sm_state *state = &table[cur_state];
    if ((uint8_t)state->count == 0) {
        return;
    }

    /* Linear scan for a live transition matching this event. The OEM
     * loads `remaining` once (not per iteration) and walks the entry
     * pointer in lock-step with the matched-entry index. */
    uint32_t remaining = (uint8_t)state->count;
    const struct sm_transition *entry = state->transitions;
    uint32_t match_index = 0;

    for (;;) {
        if (entry->handler != NULL && (uint8_t)event_id == entry->event_id) {
            break;
        }
        remaining--;
        entry++;
        match_index++;
        if (remaining == 0) {
            return;  /* no matching transition */
        }
    }

    /* Re-fetch the handler through the state-record's transitions base +
     * matched-entry offset, exactly as the OEM does, then invoke it. */
    uint32_t result = state->transitions[match_index].handler(event_id, payload, len);

    uint8_t next_state;
    if (result == SM_HANDLED) {
        cur_state = ctx[1];  /* handler may have changed the state byte */
        next_state = table[cur_state].transitions[match_index].next_state_handled;
        if (next_state == SM_STATE_NO_CHANGE) {
            return;
        }
        ctx[1] = next_state;
        return;
    }

    cur_state = ctx[1];
    next_state = table[cur_state].transitions[match_index].next_state_default;
    if (next_state == SM_STATE_NO_CHANGE) {
        return;
    }
    ctx[1] = next_state;
}

/* Queue-worker callback for state-machine events.
 *
 * Registered (via its +1 Thumb pointer, 0x2719D) as the message callback
 * by sm_dispatch_event_with_context @ 0x00017C6C. The queued message
 * carries the small envelope built on the publish side:
 *
 *     msg[0]    event_id (byte)
 *     msg[1]    pad
 *     msg[2..3] len (halfword)
 *     msg[4..]  payload
 *
 * It unpacks the header and tail-calls sm_run_dispatcher with the
 * payload pointer advanced past the 4-byte header. (The OEM loads the
 * length halfword into the dispatcher's third argument; sm_run_dispatcher
 * forwards it untouched to the matched handler.)
 *
 * OEM @ 0x0002719C — an undefined/uncarved 14-byte thunk in Ghidra. */
void sm_dispatch_event_callback(const void *msg)
{
    const uint8_t *m = (const uint8_t *)msg;
    uint16_t len;
    uint8_t  event_id;

    len      = (uint16_t)(m[2] | ((uint16_t)m[3] << 8));
    event_id = m[0];

    sm_run_dispatcher(event_id, m + 4, len);
}
