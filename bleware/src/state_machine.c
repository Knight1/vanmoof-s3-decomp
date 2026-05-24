/* state_machine.c — bleware state-machine notify primitive.
 *
 * Functions decoded:
 *
 *   `state_machine_post` @ 0x00017C6C
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
