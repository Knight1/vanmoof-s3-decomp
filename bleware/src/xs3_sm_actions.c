/* xs3_sm_actions.c — small action-layer helpers invoked by the BLE state
 * machine (src/state_machine_handlers.c) that were previously referenced by
 * raw OEM address.
 *
 * Two shared primitives these reach were investigated against the SimpleLink
 * SDK 3.40 cc26x2v2 golden ROM symbol table (docs/rom-thunk-audit.md):
 *
 *   - 0x0001AC6C (`log_emit_v`) is the generic ICall synchronous service
 *     request/reply helper (type-0x14 envelope -> send to service `param_1`
 *     -> wait 1000 ms -> return reply). Not a mislabel; the tree already uses
 *     it for GAP commands and bond reads as well as logging.
 *   - 0x00020098 (decoded as `oad_state_lock`) is actually a Clock-reconfigure
 *     helper (Clock_stop/Clock_setTimeout/Clock_setPeriod/Clock_start) — the
 *     "Semaphore_pend/HwiP" framing in oad.c IS a mislabel (SDK-confirmed). It
 *     is referenced here by its existing symbol pending an oad.c re-validation.
 *
 * The OEM source file for these helpers is not recoverable from the binary
 * (they emit no log/__func__ strings); they are grouped here by role.
 *
 * OEM addresses are noted per function.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- already-decoded / shared callees (other TUs) ------------------ */
extern int module_forward_async(uint32_t cmd_id, uint8_t byte_value);  /* FUN_00024508, ssp.c */
extern void ssp_signal_fetch(uint16_t cmd_id);                         /* FUN_00025B04, ssp.c */

/* Generic TI-BLE-stack ICall synchronous request/reply helper (the same
 * primitive log_emit.c/xs3_gap_adv.c/xs3_bond.c use): send a type-0x14
 * message carrying `payload` + up to two inline args to ICall service
 * `service_id`, wait for the reply, return the reply word. FUN_0001AC6C. */
extern uint32_t log_emit_v(uint32_t service_id, const void *payload, ...);

/* TI-RTOS Clock primitives. RESOLVED via the SDK 3.40 cc26x2v2 golden ROM
 * symbol table (docs/rom-thunk-audit.md): 0x20098 is a Clock-reconfigure
 * helper (Clock_stop?/Clock_setTimeout/Clock_setPeriod/Clock_start), and the
 * veneers 0x27D50 / 0x27BE0 are Clock_start (0x1002E9E6) / Clock_stop
 * (0x1002E2C4). The existing-tree name `oad_state_lock` for 0x20098 is a
 * confirmed mislabel (kept for now — oad.c also Semaphore_post()s the handle
 * it passes, an unresolved clock-vs-semaphore tangle needing Ghidra). */
extern void oad_state_lock(void *handle, int period_ms);  /* FUN_00020098 = clock-reconfigure */
extern void clock_start(void *handle);                    /* @ 0x00027D50 → Clock_start */

/* Connection-presence bitmask: the variable at 0x00026924 points at a
 * 32-bit word whose bit N is set while link/handle N is up. */
extern volatile uint32_t *g_ble_link_state_bits;   /* *(uint32_t **)0x00026924 */

/* Test bit `index` of the connection-presence bitmask. OEM @ 0x00026914. */
uint32_t ble_link_state_bit(uint32_t index)
{
    return (*g_ble_link_state_bits >> (index & 0xff)) & 1u;
}

/* Non-zero while a peer occupies link slot 0x0D. OEM @ 0x00026C54. */
int is_peer_connected(void)
{
    return ble_link_state_bit(0x0D) != 0;
}

/* Assert / de-assert SSP command 0xFA (a single-byte async forward). The
 * exact meaning of cmd 0xFA is not yet identified, so these are named by
 * mechanism. OEM @ 0x000275A8 (set) and 0x000275B0 (clear). */
void module_cmd_fa_set(void)
{
    module_forward_async(0xfa, 1);
}

void module_cmd_fa_clear(void)
{
    module_forward_async(0xfa, 0);
}

/* Stop a TI-RTOS Clock object and free the argument block it was carrying.
 * The clock stores its callback argument at handle+0x1C; on disarm the OEM
 * stops the clock, frees that block (if any), and clears the slot. Returns 0.
 * OEM @ 0x000261B2. (Arm counterpart: clock_arm, 0x00025F8C.) */
int clock_disarm(void *clock_handle)
{
    extern void clock_stop(void *handle);   /* @ 0x00027BE0 → Clock_stop (0x1002E2C4) */
    extern void heap_free(void *p);         /* FUN_00021B88 */

    if (clock_handle != NULL) {
        clock_stop(clock_handle);
        void *arg = *(void *volatile *)((uint8_t *)clock_handle + 0x1c);
        if (arg != NULL) {
            heap_free(arg);
            *(void *volatile *)((uint8_t *)clock_handle + 0x1c) = NULL;
        }
    }
    return 0;
}

/* When `conn` is the connection that currently owns the OAD session (tracked
 * by the global active-OAD-handle, 0xFFFF = none), post control event 0x15
 * and tear the session down. No-op otherwise. OEM @ 0x000254B4. */
void oad_close_for_conn(uint16_t conn)
{
    extern volatile uint16_t *g_oad_active_conn;              /* *(uint16_t **)0x000254D4 */
    extern void bleware_control_event_post(uint32_t status); /* FUN_000265C4, oad.c */
    extern void oad_session_close(void);                     /* FUN_00025060, oad.c */

    if (*g_oad_active_conn != 0xffff && *g_oad_active_conn == conn) {
        bleware_control_event_post(0x15);
        oad_session_close();
    }
}

/* Arm a one-shot TI-RTOS timer: set its period (ms), stash the (heap-owned)
 * callback argument at handle+0x1C — freed by clock_disarm — and start it.
 * Returns 0, or 1 if the handle is NULL. OEM @ 0x00025F8C. */
int clock_arm(uint32_t clock_handle, int period_ms, void *arg)
{
    void *h = (void *)(uintptr_t)clock_handle;
    if (h == NULL) {
        return 1;
    }
    oad_state_lock(h, period_ms);   /* 0x20098 — set-period (see note above) */
    *(void *volatile *)((uint8_t *)h + 0x1c) = arg;
    clock_start(h);
    return 0;
}

/* GAP advertising "force" state set: when `flag` differs from the cached state
 * byte (state+3), push GAP stack command 0x10A, then cache it. OEM @ 0x00023800. */
void gap_adv_state_set(int flag)
{
    extern uint8_t        *g_gap_adv2_state;    /* DAT_00023830 (record ptr) */
    extern const uint8_t   g_gap_cmd_10a_desc[];/* DAT_00023834 */
    uint8_t local = (uint8_t)flag;

    if ((char)local != (char)g_gap_adv2_state[3]) {
        log_emit_v(0x10, g_gap_cmd_10a_desc, 0x10a, 1, &local);
    }
    g_gap_adv2_state[3] = local;
}

/* Issue GAP stack command 0x409 (no args). Returns 0 on success, -1 if the
 * ICall request failed. OEM @ 0x00025400. */
int gap_terminate_request(void)
{
    extern const uint8_t g_gap_cmd_409_desc[];   /* DAT_00025420 */
    return log_emit_v(0x10, g_gap_cmd_409_desc, 0x409, 0) == 0 ? 0 : -1;
}

/* Post GAP stack command 0x13 (a per-connection link/notify failure signal)
 * for `conn`. OEM @ 0x00026FC4. */
void ble_post_link_error(uint16_t conn)
{
    extern const uint8_t g_gap_cmd_13_desc[];   /* DAT_00026FD0 */
    log_emit_v(0x10, g_gap_cmd_13_desc, conn, 0x13);
}

/* Advertising keep-alive gate. On an event whose byte has bit 2 set, and only
 * while the gate is idle (record+0 == 0), (re)start a 500 ms keep-alive timer
 * on the record's clock (record+0xC), bump the counter (record+8), and on the
 * first pulse poke SSP cmd 0x5521. Returns the running count / the event's
 * high bits. OEM @ 0x0002200C. */
uint32_t advert_keepalive_pulse(const uint8_t *ev)
{
    extern uint8_t *g_advert_gate;    /* DAT_00022048: +0 active, +8 counter, +0xC clock */
    extern uint8_t *g_advert_other;   /* DAT_0002204C */
    uint32_t v = ev[0] >> 3;

    if ((ev[0] >> 2) & 1) {
        v = *(uint32_t *)(g_advert_gate + 8);
        if (g_advert_gate[0] == 0) {
            if (v == 0) {
                *g_advert_other = 0xff;
                ssp_signal_fetch(0x5521);
                v = *(uint32_t *)(g_advert_gate + 8);
            }
            *(uint32_t *)(g_advert_gate + 8) = v + 1;
            oad_state_lock(*(void **)(g_advert_gate + 0xc), 500);
            clock_start(*(void **)(g_advert_gate + 0xc));
            v = 1;
            g_advert_gate[0] = 1;
        }
    }
    return v;
}

/* Event-payload helper used by sm_handler_ev03: when the event byte has bit 2
 * set, clear the global one-byte ack flag and return its address; otherwise
 * return the event byte's high bits. The sole caller discards the result.
 * OEM @ 0x00026A40. */
uint8_t *event_payload_ack(uint8_t *ev)
{
    extern uint8_t *g_event_ack_flag;   /* DAT_00026A50 */
    uint8_t *result = (uint8_t *)(uintptr_t)(ev[0] >> 3);

    if ((ev[0] >> 2) & 1) {
        *g_event_ack_flag = 0;
        result = g_event_ack_flag;
    }
    return result;
}
