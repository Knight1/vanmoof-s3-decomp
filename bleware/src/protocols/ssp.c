/* protocols/ssp.c — System Service Protocol transport primitives.
 *
 * OEM source path observed:  `source/protocols/ssp.c`
 * (embedded in `monitor_log` strings inside `ssp_publish_fetch_frame`:
 *   "SSP no init", "SSP Out of memory")
 *
 * "SSP" is the OEM's name for the inter-module bus that the central
 * GATT dispatchers bridge BLE writes/reads onto. Every char that
 * isn't OAD/log/backoffice gets bridged via the helpers here.
 *
 * Three callable transport primitives translated below:
 *
 *   `module_forward_async(cmd_id, byte)`       — 1-byte async forward
 *   `module_publish_command(cmd, payload, len)` — N-byte async publish
 *   `module_publish_sync_with_timeout(...)`     — sync FETCH + wait
 *
 * Frame structure (39-byte header + payload):
 *
 *   +0x0C  u32 context_word_1   (param_5)
 *   +0x10  u32 context_word_2   (param_6)
 *   +0x14  u32 timeout          = 5
 *   +0x18  u32 retry            = 100
 *   +0x20  u8  frame_type       = 2
 *   +0x21  u8  priority         = 7 (publish) / 6 (fetch)
 *   +0x22  u8  sequence
 *   +0x23  u16 cmd_id
 *   +0x25  u16 payload_len
 *   +0x27  u8[len] payload
 *
 * Per-module reply state: an array at RAM `0x20004158` (= flash
 * `DAT_0001EF4C`), 0x7C bytes per module:
 *
 *   +0x44 reply semaphore handle
 *   +0x54 u16 cmd-currently-pending (0xFFFF = idle)
 *   +0x79 u8  "bus stuck" flag (set on previous timeout; raises this
 *             call's timeout to 500 ms and clears itself)
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

#define SSP_FRAME_HEADER_LEN 0x27u
#define SSP_MAX_PAYLOAD_LEN  0x100u

#define SSP_FRAME_TYPE       2u
#define SSP_PRIO_PUBLISH     7u
#define SSP_PRIO_FETCH       6u

#define SSP_DEFAULT_TIMEOUT  5u
#define SSP_DEFAULT_RETRY    100u

#define SSP_MODULE_REC_STRIDE      0x7C
#define SSP_MODULE_REC_REPLY_SEM   0x44
#define SSP_MODULE_REC_PENDING_CMD 0x54
#define SSP_MODULE_REC_STUCK_FLAG  0x79

extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);
extern void ti_mailbox_post  (uint32_t mbox,  void    *msg);
extern void ti_semaphore_post(uint32_t handle);
extern void *ssp_frame_alloc(uint32_t size);
extern void  gpio_write(void *gpio_ctx, int dio, int value);
extern void *memcpy(void *dst, const void *src, unsigned int n);

/* ble_connection_get_session_key — declared in bleware.h */

struct ssp_master_state {
    void    *tx_queue;
    uint8_t  _pad04[0x1C];
    uint32_t tx_mailbox;          /* +0x20 — used as `param_1[8]` in the OEM code */
};
extern struct ssp_master_state g_ssp_master;      /* RAM 0x20003104 */
extern uint8_t                 g_ssp_modules[];   /* RAM 0x20004158 — per-module records */
extern uint8_t                 g_ssp_sequence;    /* `*DAT_0001816C` */
extern void                   *g_bleware_gpio_ctx;/* `DAT_00027010 + 8` */

int ble_authenticated_connection_count(void)
{
    int count = 0;
    for (int i = 0; i < 3; i++) {
        if (ble_connection_get_session_key(i) != 0) {
            count++;
        }
    }
    return count;
}

void ble_activity_led_pulse(void)
{
    gpio_write(g_bleware_gpio_ctx, 0xD, 1);
}

/* Build a publish frame and enqueue it.
 *
 * Returns 0 on success; negative on error:
 *   -5 (`0xFFFFFFFB`) — queue not initialised
 *   -3 (`0xFFFFFFFD`) — payload too large (> 256 bytes)
 *   -1 (`0xFFFFFFFF`) — alloc failed
 *
 * OEM @ 0x000180D0.
 */
int ssp_queue_publish_frame(struct ssp_master_state *master,
                            uint16_t                 cmd_id,
                            const void              *payload,
                            uint32_t                 payload_len,
                            uint32_t                 ctx1,
                            uint32_t                 ctx2,
                            uint8_t                 *seq_out)
{
    if (master->tx_queue == NULL) {
        return -5;
    }
    if (payload_len > SSP_MAX_PAYLOAD_LEN) {
        return -3;
    }

    uint8_t *frame = (uint8_t *)ssp_frame_alloc(
        (payload_len + SSP_FRAME_HEADER_LEN) & 0xFFFFu);
    if (frame == NULL) {
        return -1;
    }

    *(uint32_t *)(frame + 0x0C) = ctx1;
    *(uint32_t *)(frame + 0x10) = ctx2;
    *(uint32_t *)(frame + 0x14) = SSP_DEFAULT_TIMEOUT;
    *(uint32_t *)(frame + 0x18) = SSP_DEFAULT_RETRY;
    frame[0x20] = SSP_FRAME_TYPE;
    frame[0x21] = SSP_PRIO_PUBLISH;

    uint8_t seq = g_ssp_sequence;
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    frame[0x22] = seq;
    *(uint16_t *)(frame + 0x23) = cmd_id;
    *(uint16_t *)(frame + 0x25) = (uint16_t)payload_len;
    memcpy(frame + 0x27, payload, payload_len);

    g_ssp_sequence = (uint8_t)(seq + 1);

    /* Take the mailbox lock, post the frame, release.
     * `master[+0x20] = tx_mailbox` is the TI-RTOS Mailbox handle —
     * indexed as `param_1[8]` in the OEM lift because the struct
     * starts with `tx_queue` (4 B) then padding to +0x20. */
    ti_semaphore_pend(master->tx_mailbox, 0xFFFFFFFFu);
    ti_mailbox_post  ((uint32_t)master->tx_queue, frame);
    ti_semaphore_post(master->tx_mailbox);
    return 0;
}

/* Send a 1-byte async forward on the SSP bus.
 *
 * Used by the central GATT write dispatcher when the BLE char's
 * payload is a single byte (the common case for most bridged chars).
 *
 * OEM @ 0x00024508.
 */
int module_forward_async(uint32_t cmd_id, uint8_t byte_value)
{
    uint8_t payload[1] = { byte_value };

    if (ble_authenticated_connection_count() != 0) {
        ble_activity_led_pulse();
    }

    return ssp_queue_publish_frame(&g_ssp_master, (uint16_t)cmd_id,
                                   payload, 1, 0, 0, NULL);
}

/* Send an N-byte async command on the SSP bus.
 *
 * Used by characteristics whose write payload is non-trivial (e.g.,
 * `cmd_audio_volume_set_all` posts a 12-byte mask, the backoffice
 * dispatcher posts 9-cmd-specific blobs, etc).
 *
 * OEM @ 0x000244D8.
 */
int module_publish_command(uint16_t cmd, const uint8_t *payload, unsigned int len)
{
    if (ble_authenticated_connection_count() != 0) {
        ble_activity_led_pulse();
    }

    return ssp_queue_publish_frame(&g_ssp_master, cmd, payload, len,
                                   0, 0, NULL);
}

/* Trigger a synchronous FETCH on the SSP bus for `cmd_id`, then pend
 * on the per-module reply semaphore.
 *
 * Used by the central GATT *read* dispatcher when a characteristic is
 * marked "refresh from SSP before serving" (entry +0x1A non-zero).
 *
 * If a previous fetch timed out (stuck flag set at +0x79), this call's
 * timeout is bumped to 500 ms and the flag is cleared — a one-shot
 * grace period to let a slow slave catch up.
 *
 * Returns 0 on reply, -1 on timeout.
 *
 * OEM @ 0x0001EEF4.
 */
void ssp_signal_fetch(uint16_t cmd_id);   /* defined below */

int module_publish_sync_with_timeout(uint32_t module_idx,
                                     uint16_t cmd_id,
                                     uint32_t timeout_ms)
{
    uint8_t *rec = g_ssp_modules + module_idx * SSP_MODULE_REC_STRIDE;
    uint32_t timeout = timeout_ms;

    if (rec[SSP_MODULE_REC_STUCK_FLAG] == 1) {
        timeout = 500;
        rec[SSP_MODULE_REC_STUCK_FLAG] = 0;
    }

    /* Drain any stale wake-ups on the reply semaphore. */
    while (ti_semaphore_pend(*(uint32_t *)(rec + SSP_MODULE_REC_REPLY_SEM), 0) == 1) {
    }

    *(uint16_t *)(rec + SSP_MODULE_REC_PENDING_CMD) = cmd_id;
    ssp_signal_fetch(cmd_id);

    /* OEM converts `timeout_ms` to "100 µs ticks" by multiplying by
     * 100 — TI-RTOS Clock_tickPeriod on this build is 10 µs. */
    int rc = ti_semaphore_pend(
        *(uint32_t *)(rec + SSP_MODULE_REC_REPLY_SEM),
        timeout * 100u);

    *(uint16_t *)(rec + SSP_MODULE_REC_PENDING_CMD) = 0xFFFFu;
    return (rc == 1) ? 0 : -1;
}

/* Tail-half of the sync fetch: pulses the activity LED, then calls
 * `ssp_publish_fetch_frame` to enqueue a FETCH-shaped frame
 * (priority 6, no payload) for `cmd_id`. The slave that owns `cmd_id`
 * will see the FETCH and reply by publishing the current value, which
 * unblocks the reply-semaphore wait in `module_publish_sync_with_timeout`.
 *
 * OEM @ 0x00025B04.
 */
/* Forward declare — defined below. */
int ssp_publish_fetch_frame(struct ssp_master_state *master,
                            uint16_t cmd_id, uint8_t *seq_out,
                            uint32_t extra);

void ssp_signal_fetch(uint16_t cmd_id)
{
    if (ble_authenticated_connection_count() != 0) {
        ble_activity_led_pulse();
    }
    ssp_publish_fetch_frame(&g_ssp_master, cmd_id, NULL, 0);
}

/* FETCH-frame variant of `ssp_queue_publish_frame`. Differences:
 *   - priority 6 instead of 7 (RX-side urgency is higher)
 *   - context word at +0x0C is zeroed (no caller context)
 *   - no payload (len always 0)
 *   - logs "SSP no init" on uninitialised queue, "SSP Out of memory"
 *     on alloc failure (via the `source/protocols/ssp.c` log path)
 *
 * OEM @ 0x00015D24.
 */
int ssp_publish_fetch_frame(struct ssp_master_state *master,
                            uint16_t                 cmd_id,
                            uint8_t                 *seq_out,
                            uint32_t                 extra)
{
    if (master->tx_queue == NULL) {
        monitor_log("source/protocols/ssp.c", 0x1B8,
                    "ssp_publish_fetch_frame", 2, "SSP no init");
        return -5;
    }

    uint8_t *frame = (uint8_t *)ssp_frame_alloc(SSP_FRAME_HEADER_LEN);
    if (frame == NULL) {
        monitor_log("source/protocols/ssp.c", 0x1C4,
                    "ssp_publish_fetch_frame", 2, "SSP Out of memory");
        return -1;
    }

    uint8_t seq = g_ssp_sequence;
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    *(uint16_t *)(frame + 0x23) = cmd_id;
    *(uint32_t *)(frame + 0x14) = SSP_DEFAULT_TIMEOUT;
    frame[0x22] = seq;
    *(uint32_t *)(frame + 0x18) = SSP_DEFAULT_RETRY;
    frame[0x21] = SSP_PRIO_FETCH;
    *(uint32_t *)(frame + 0x0C) = 0;
    frame[0x20] = SSP_FRAME_TYPE;

    g_ssp_sequence = (uint8_t)(seq + 1);

    ti_semaphore_pend(master->tx_mailbox, 0xFFFFFFFFu);
    ti_mailbox_post  ((uint32_t)master->tx_queue, frame);
    ti_semaphore_post(master->tx_mailbox);
    (void)extra;
    return 0;
}

/* Check whether a Modbus/SSP transaction is currently in-flight.
 * Returns 0 if the bus is idle (pending-cmd word == 0xFFFF), 1 if busy.
 * OEM at 0x00026594 (18 B). */
int module_bus_is_idle(void)
{
    extern uint16_t *g_ssp_bus_pending_cmd_ptr;  /* DAT_000265A8 = 0x20009A90 */
    return (*g_ssp_bus_pending_cmd_ptr == 0xFFFFu) ? 0 : 1;
}

/* Synchronous forward — publish a command payload and block until the
 * reply comes back. Used by the backoffice handler for sub-command 8
 * (module-forward-sync). Returns 0 on reply received, -1 on timeout.
 * OEM @ 0x000177E8 (72 B).
 *
 * The OEM does NOT wait on the global per-module record semaphore.
 * Instead it constructs a LOCAL TI-RTOS Semaphore on the stack and
 * threads its address into the publish frame's context words so the
 * reply path posts exactly THIS call's semaphore:
 *
 *   ctx1 (+0x0C) = reply-callback pointer (flash 0x00027054, Thumb).
 *                  On reply it does: handle = ctx2[0]; ctx2[4] = status;
 *                  if (handle) Semaphore_post(handle).
 *   ctx2 (+0x10) = &reply_ctx — { sem_handle; status_byte }.
 *   seq_out      = &seq — frame sequence number, used to dequeue the
 *                  stale frame on timeout.
 *
 * The reply callback overwrites reply_ctx.status (pre-seeded to -4) with
 * the slave's status byte; that byte is the call's return value on a
 * successful pend. On timeout the queued frame is removed by sequence
 * number and the call returns -1. */
int module_forward_sync(uint16_t cmd_id, const uint8_t *payload,
                        unsigned int len)
{
    /* TI-RTOS Semaphore lifecycle (ROM thunks). */
    extern void  ti_semaphore_params_init(void *params, uint32_t a1,
                                          uint32_t size, uint32_t a3);
    extern void  ti_semaphore_construct(void *obj, uint32_t count,
                                        const void *params);
    extern void  ti_semaphore_destruct(void *obj);
    /* Remove the queued (but never-acked) frame by sequence number on
     * timeout. OEM FUN_00021aba. */
    extern void  ssp_dequeue_frame_by_seq(struct ssp_master_state *master,
                                          uint8_t seq);
    /* Internal SSP reply router at flash 0x00027054 — carried as ctx1
     * so the slave's reply posts our local semaphore. */
    extern void  ssp_sync_reply_route(void);
    /* TI-RTOS tick period in µs (10). OEM `*DAT_00017888` = *0x0002BB88. */
    extern uint32_t *g_tick_period_ptr;

    /* Stack-resident Semaphore object + params, matching the OEM frame
     * (params 0x24 bytes, object 0x1C bytes). The constructed object's
     * own address is the handle the reply path posts. */
    uint8_t   sem_params[0x24];
    uint8_t   sem_obj[0x1C];
    struct { void *sem; signed char status; } reply_ctx;
    uint8_t   seq = 0;
    int       rc;
    int       result;

    ti_semaphore_params_init(sem_params, 0, 0x24, 8);
    sem_params[0x18] = 1;   /* OEM `local_40 = 1` (mode/event field) */
    ti_semaphore_construct(sem_obj, 0, sem_params);

    reply_ctx.sem    = sem_obj;
    reply_ctx.status = -4;   /* sentinel; overwritten by the reply route */

    if (ble_authenticated_connection_count() != 0) {
        ble_activity_led_pulse();
    }

    ssp_queue_publish_frame(&g_ssp_master, cmd_id, payload, len,
                            (uint32_t)(uintptr_t)ssp_sync_reply_route,
                            (uint32_t)(uintptr_t)&reply_ctx, &seq);

    /* OEM timeout = (1000 / tick_us) * 2500 ticks (=250000 at 10µs). */
    rc = ti_semaphore_pend((uint32_t)(uintptr_t)reply_ctx.sem,
                           (1000u / *g_tick_period_ptr) * 2500u);
    if (rc == 1) {
        result = reply_ctx.status;
    } else {
        ssp_dequeue_frame_by_seq(&g_ssp_master, seq);
        result = -1;
    }

    ti_semaphore_destruct(sem_obj);
    return result;
}
