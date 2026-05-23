/* ti_rtos_msgq.c — wrap a Queue_put + Event_post pair.
 *
 * OEM symbol: `task_queue_enqueue_and_signal` @ 0x00023982.
 *
 * Allocates a 12-byte Queue_Elem node from the heap, stashes the
 * caller's payload pointer at node+8, posts the node onto the TI-RTOS
 * Queue, and (if `event` is non-NULL) signals event-flag bit 30 on the
 * waiting task. On allocation failure the payload is freed and 0 is
 * returned; success returns 1.
 *
 * Used as a one-shot primitive by every "post a kind-N message to the
 * bluetoothtask" helper (see src/bluetoothtask_post.c) and by the
 * ICall-reply path that wakes log_emit_v's caller (FUN_0001134c).
 */

#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* TI-RTOS Kernel ROM thunks. */
extern void ti_queue_put (void *queue, void *elem);     /* @ 0x00027900 → ROM 0x1002E96C */
extern void ti_event_post(void *event, uint32_t flags); /* @ 0x00027C70 → ROM 0x1002CFB2 */

#define TASK_EVENT_FLAG_USER_MSG  0x40000000u

int task_queue_enqueue_and_signal(void *queue, void *event, void *payload)
{
    struct queue_elem {
        struct queue_elem *next;     /* +0x00 — populated by Queue_put */
        struct queue_elem *prev;     /* +0x04 — populated by Queue_put */
        void              *payload;  /* +0x08 — caller payload */
    } *node;

    node = (struct queue_elem *)monitor_alloc(sizeof *node);
    if (node == NULL) {
        monitor_free(payload);
        return 0;
    }
    node->payload = payload;
    ti_queue_put(queue, node);
    if (event != NULL) {
        ti_event_post(event, TASK_EVENT_FLAG_USER_MSG);
    }
    return 1;
}
