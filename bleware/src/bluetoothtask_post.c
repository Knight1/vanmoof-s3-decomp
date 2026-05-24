/* bluetoothtask_post.c — helpers that post inter-task messages to the
 * bluetoothtask's user-message queue.
 *
 * The bluetoothtask drains a small set of "kind"-tagged messages from
 * its user-message queue when event-flag bit 30 (0x40000000) is set
 * (see bluetoothtask_main, progress.md). Each kind has its own
 * payload struct; this TU provides the per-kind posters.
 *
 * Wire format:
 *   - Heap envelope (8 B): { u8 kind, pad x3, void *payload }
 *   - Heap payload (size depends on kind)
 *
 * The envelope and payload are both allocated from the TI-RTOS heap
 * via `monitor_alloc`. The bluetoothtask handler is responsible for
 * freeing them after consumption.
 */

#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* ---- Bluetoothtask message-queue control struct ---------------- *
 * RAM `g_bluetoothtask_msg_queue` (= flash 0x00021078 → 0x200057C8).
 * Used by every poster to find the TI-RTOS Queue + Event handles
 * the bluetoothtask is pending on. */
struct bluetoothtask_queue {
    uint32_t  reserved0;       /* +0x00 — likely role byte at 0x200057C9 */
    void     *event_handle;    /* +0x04 — Event_post bit 30 wakes the task */
    void     *queue_handle;    /* +0x08 — Queue_put deposits the envelope */
};
extern struct bluetoothtask_queue * const g_bluetoothtask_msg_queue;

/* Envelope laid down on the queue (8 B). */
struct bt_msg_envelope {
    uint8_t  kind;          /* +0x00 */
    uint8_t  pad[3];
    void    *payload;       /* +0x04 */
};

/* Kind 0x04 — request disconnect.
 * Payload (4 B): { u8 reason, u8 pad, u16 conn_handle }. */
struct bt_disconnect_payload {
    uint8_t  reason;        /* +0x00 — error/reason code */
    uint8_t  pad;
    uint16_t conn;          /* +0x02 — connection handle (0xFFFD = "all") */
};

#define BT_MSG_KIND_DISCONNECT  0x04

/* TI-RTOS heap (same allocator as `monitor_alloc`/`monitor_free`). */
extern void *monitor_alloc(unsigned int size);
extern void  monitor_free(void *p);

/* Queue-put + event-post combo. Allocates a 12-B Queue_Elem node,
 * stashes `payload` at node+8, runs Queue_put, then Event_post
 * (bit 30) on `event` if non-NULL. Returns 1 on success, 0 if the
 * node alloc fails (in which case `payload` is freed). */
extern int   task_queue_enqueue_and_signal(void *queue, void *event,
                                           void *payload);   /* FUN_00023982 */

int ble_post_disconnect(uint16_t conn, uint8_t reason)
{
    struct bt_disconnect_payload *pl;
    struct bt_msg_envelope       *env;

    pl = (struct bt_disconnect_payload *)monitor_alloc(sizeof *pl);
    if (pl == NULL) {
        return -1;
    }
    pl->conn   = conn;
    pl->reason = reason;

    env = (struct bt_msg_envelope *)monitor_alloc(sizeof *env);
    if (env != NULL) {
        env->kind    = BT_MSG_KIND_DISCONNECT;
        env->payload = pl;
        if (task_queue_enqueue_and_signal(g_bluetoothtask_msg_queue->queue_handle,
                                          g_bluetoothtask_msg_queue->event_handle,
                                          env) != 0) {
            return 0;
        }
    }
    monitor_free(pl);
    return -1;
}

/* Allocate an 8-byte control envelope {kind, payload_ptr} and enqueue it
 * to the bluetoothtask's user-message queue. On failure returns 0x13
 * (alloc failed). On success, task_queue_enqueue_and_signal takes care
 * of posting the event bit 30 to wake the bluetoothtask.
 * OEM @ 0x00023CC8 (48 B). */
int task_queue_publish_envelope(uint32_t kind, const void *payload,
                                uint16_t len, uint32_t tag)
{
    struct bt_msg_envelope *env;
    struct bluetoothtask_queue *mq;

    (void)len;   /* unused by this wrapper — caller pre-sizes the payload */
    (void)tag;   /* tag is consumed by the caller, not the envelope */

    env = (struct bt_msg_envelope *)monitor_alloc(sizeof *env);
    if (env == NULL) {
        return 0x13;
    }

    mq = g_bluetoothtask_msg_queue;
    env->kind    = (uint8_t)kind;
    env->payload = (void *)payload;

    return task_queue_enqueue_and_signal(mq->queue_handle,
                                         mq->event_handle, env) ? 0 : 0x13;
}
