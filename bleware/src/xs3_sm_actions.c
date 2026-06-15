/* xs3_sm_actions.c — small action-layer helpers invoked by the BLE state
 * machine (src/state_machine_handlers.c) that were previously referenced by
 * raw OEM address.
 *
 * Only the helpers whose entire call graph resolves to already-decoded /
 * unambiguous symbols are decoded here. Several sibling helpers
 * (FUN_00025400, FUN_00023800, FUN_00026FC4, FUN_000211F8, FUN_0002200C,
 * and the large xs3_app functions) call two shared primitives that the
 * existing tree appears to mislabel, so they are deferred until those are
 * reconciled:
 *
 *   - 0x0001AC6C — decoded as `log_emit_v` (variadic logger) in
 *     src/log_emit.c, but the body is a *generic* ICall synchronous
 *     service request (type-0x14 envelope -> send to service `param_1`
 *     -> wait 1000 ms -> return the reply word). The connection-count and
 *     GAP-state paths call it as a request, not a log.
 *   - 0x00020098 — defined as `oad_state_lock` (Semaphore_pend) in
 *     src/oad.c, but the body is a clock-set-period helper (Clock_setTimeout
 *     in ticks, then restart) called by the SM clock-arm path.
 *
 * The OEM source file for these helpers is not recoverable from the binary
 * (they emit no log/__func__ strings); they are grouped here by role.
 *
 * OEM addresses are noted per function.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- already-decoded callees (other TUs) --------------------------- */
extern int module_forward_async(uint32_t cmd_id, uint8_t byte_value);  /* FUN_00024508, ssp.c */

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
 * OEM @ 0x000261B2. (Its arm counterpart, 0x00025F8C, calls the
 * apparently-mislabeled 0x00020098 and is deferred — see file header.) */
int clock_disarm(void *clock_handle)
{
    extern void clock_stop(void *handle);   /* @ 0x00027BE0 — ROM Clock_stop veneer */
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
