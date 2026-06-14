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
/* DAT_00017D08 — the OEM __func__ string for this function. */
#define STATE_MACHINE_LOG_FN       "sm_dispatch_event_with_context"
#define STATE_MACHINE_SRC_FILE     "source/xs3_statemachine.c"
#define STATE_MACHINE_LOG_LINE     0x184         /* line 388 in the OEM source */

void state_machine_post(uint32_t state_id, const void *payload, uint16_t len)
{
    uint16_t total = (uint16_t)(len + 4u);
    uint8_t *buf   = monitor_alloc(total);
    if (buf == NULL) {
        monitor_log(STATE_MACHINE_SRC_FILE, STATE_MACHINE_LOG_LINE,
                    STATE_MACHINE_LOG_FN, 2,
                    "Could not allocate memory for XS3 event %d", state_id);
        firmware_abort();
    }

    buf[0] = (uint8_t)state_id;
    /* buf[1] is implicit padding. */
    buf[2] = (uint8_t)(len     );
    buf[3] = (uint8_t)(len >> 8);
    memcpy(buf + 4, payload, len);

    /* The 4th argument is the queue-worker callback the bluetoothtask
     * invokes to consume this message — OEM DAT_00017D0C holds
     * &sm_dispatch_event_callback with the Thumb bit (0x0002719D), NOT an
     * opaque tag. task_queue_publish_envelope stores it as the envelope's
     * first word, which the consumer calls through. */
    task_queue_publish_envelope(STATE_MACHINE_QUEUE_KIND, buf, total,
                                (uint32_t)(uintptr_t)&sm_dispatch_event_callback);
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
 * current-state byte is at context[1]. Both are defined at the bottom of
 * this file; forward-declared here so the dispatcher can reference them. */
extern const struct sm_state  g_sm_state_table[];   /* 0x0002B228 */
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
        if (entry->handler != NULL && event_id == entry->event_id) {
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

/* ---------------------------------------------------------------------
 * Materialized state table (OEM flash data)
 * ---------------------------------------------------------------------
 *
 * Pinned reconstruction of the flat state machine's data, read verbatim
 * from the OEM image:
 *
 *   g_sm_state_table       @ 0x0002B228  (2 records, 16 bytes)
 *   g_sm_transitions_state0 @ 0x0002BB14 (1 transition)
 *   g_sm_transitions_state1 @ 0x00029EF8 (25 transitions)
 *
 * Reachability: state 0's only transition (event 0) advances to state 1
 * on both the HANDLED and the default branch; every state-1 transition
 * carries next_state == SM_STATE_NO_CHANGE (2) in both byte fields, so the
 * machine settles in state 1 and never leaves. No state byte above 1 is
 * ever produced, so the table is exactly 2 records — even though the
 * superseded weak placeholder reserved room for 8 (0x40 bytes).
 *
 * Every transition handler is an uncarved Thumb thunk in the OEM image
 * (no Ghidra symbol, reachable only through these pointers). Each is
 * referenced below by its raw odd (Thumb) flash address with a cast and a
 * FUN_xxxx tag. The handlers all return the 32-bit HANDLED sentinel
 * 0x03000000 on the success path; the dispatcher reads next_state_handled
 * (+1) for that case and next_state_default (+2) otherwise.
 *
 * The handler initializers are integer-to-pointer casts (a GCC-accepted
 * address-constant extension for static initializers), so the arrays stay
 * const to mirror the OEM .rodata placement.
 */

/* Cast a raw OEM Thumb code address to the state-machine handler type.
 * Bit 0 (the Thumb state bit) is retained exactly as stored in flash. */
#define SM_HANDLER(addr) \
    ((uint32_t (*)(uint32_t, const void *, uint32_t))(uintptr_t)(addr))

/* g_sm_transitions_state0 @ OEM flash 0x0002BB14 — single transition.
 * event 0 -> handler FUN_00025944, advance to state 1 on both branches. */
const struct sm_transition g_sm_transitions_state0[1] = {
    { 0x00, 0x01, 0x01, 0x00, SM_HANDLER(0x00025945) },  /* FUN_00025944 */
};

/* g_sm_transitions_state1 @ OEM flash 0x00029EF8 — 25 transitions, all
 * with next_state == 2 (SM_STATE_NO_CHANGE) so they never leave state 1. */
const struct sm_transition g_sm_transitions_state1[25] = {
    { 0x01, 0x02, 0x02, 0x00, SM_HANDLER(0x00027425) },  /* FUN_00027424 */
    { 0x02, 0x02, 0x02, 0x00, SM_HANDLER(0x00027165) },  /* FUN_00027164 */
    { 0x03, 0x02, 0x02, 0x00, SM_HANDLER(0x00027173) },  /* FUN_00027172 */
    { 0x04, 0x02, 0x02, 0x00, SM_HANDLER(0x00027181) },  /* FUN_00027180 */
    { 0x05, 0x02, 0x02, 0x00, SM_HANDLER(0x00024b0d) },  /* FUN_00024b0c */
    { 0x06, 0x02, 0x02, 0x00, SM_HANDLER(0x000261e7) },  /* FUN_000261e6 */
    { 0x0f, 0x02, 0x02, 0x00, SM_HANDLER(0x00026201) },  /* FUN_00026200 */
    { 0x10, 0x02, 0x02, 0x00, SM_HANDLER(0x00024ceb) },  /* FUN_00024cea */
    { 0x12, 0x02, 0x02, 0x00, SM_HANDLER(0x00026c8b) },  /* FUN_00026c8a */
    { 0x13, 0x02, 0x02, 0x00, SM_HANDLER(0x00027401) },  /* FUN_00027400 */
    { 0x14, 0x02, 0x02, 0x00, SM_HANDLER(0x00025c77) },  /* FUN_00025c76 */
    { 0x15, 0x02, 0x02, 0x00, SM_HANDLER(0x00026c9d) },  /* FUN_00026c9c */
    { 0x16, 0x02, 0x02, 0x00, SM_HANDLER(0x00026c67) },  /* FUN_00026c66 */
    { 0x17, 0x02, 0x02, 0x00, SM_HANDLER(0x00026c79) },  /* FUN_00026c78 */
    { 0x18, 0x02, 0x02, 0x00, SM_HANDLER(0x0001d7ad) },  /* FUN_0001d7ac */
    { 0x19, 0x02, 0x02, 0x00, SM_HANDLER(0x00026089) },  /* FUN_00026088 */
    { 0x1a, 0x02, 0x02, 0x00, SM_HANDLER(0x00024d3f) },  /* FUN_00024d3e */
    { 0x1b, 0x02, 0x02, 0x00, SM_HANDLER(0x000273e9) },  /* FUN_000273e8 */
    { 0x1c, 0x02, 0x02, 0x00, SM_HANDLER(0x000271c7) },  /* FUN_000271c6 */
    { 0x1d, 0x02, 0x02, 0x00, SM_HANDLER(0x000271b9) },  /* FUN_000271b8 */
    { 0x1e, 0x02, 0x02, 0x00, SM_HANDLER(0x00026ab9) },  /* FUN_00026ab8 */
    { 0x1f, 0x02, 0x02, 0x00, SM_HANDLER(0x00027157) },  /* FUN_00027156 */
    { 0x20, 0x02, 0x02, 0x00, SM_HANDLER(0x000273c5) },  /* FUN_000273c4 */
    { 0x23, 0x02, 0x02, 0x00, SM_HANDLER(0x000273b9) },  /* FUN_000273b8 */
    { 0x24, 0x02, 0x02, 0x00, SM_HANDLER(0x00027431) },  /* FUN_00027430 */
};

/* g_sm_state_table @ OEM flash 0x0002B228 — 2 state records.
 * Indexed by g_sm_context[1] (the current-state byte). */
const struct sm_state g_sm_state_table[2] = {
    { g_sm_transitions_state0,  1 },   /* state 0 */
    { g_sm_transitions_state1, 25 },   /* state 1 */
};

/* g_sm_context @ RAM 0x20005950 — the live state-machine context block.
 * Only context[1] (the current-state byte) is touched by the dispatcher;
 * the OEM reserves 4 bytes here. */
volatile uint8_t g_sm_context[4];
