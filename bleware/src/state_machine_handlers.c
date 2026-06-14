/* state_machine_handlers.c — the 26 per-event transition handlers.
 *
 * These are the action functions referenced by the flat state-machine
 * tables in state_machine.c (g_sm_transitions_state0 / _state1). Each is
 * invoked by sm_run_dispatcher (OEM 0x00019C30) for one (state, event)
 * pair. Previously the tables held raw odd (Thumb) flash addresses behind
 * SM_HANDLER() casts because the handlers were uncarved in Ghidra; this
 * file decodes each to faithful C so the tables reference real symbols.
 *
 * Handler ABI (as the OEM dispatcher invokes them):
 *
 *     handler(r0 = event_id, r1 = payload, r2 = len, r3 = <handler addr>)
 *
 * The dispatcher does `mov r3,r1; ...; mov r3,<handler>; blx r3`, so r3 on
 * entry is the handler's OWN address — a calling-convention artifact, never
 * a real fourth argument. Ghidra therefore surfaces a dead `param_4` in a
 * few bodies (its value is the handler pointer); those reads are elided
 * here, and callees are given their true (narrower) arities. The signature
 * that matters is (event_id, payload, len).
 *
 * Almost every handler returns the 32-bit HANDLED sentinel 0x03000000 (the
 * dispatcher compares the result against it to pick next_state_handled vs
 * next_state_default). The few that do real work do it first, then return
 * the sentinel.
 *
 * The handlers reach into many subsystems (audio, GAP advertising, GATT,
 * bonding, connection params). Callees that live in other translation
 * units are referenced by their decoded symbol; leaves not yet decoded are
 * referenced by their OEM `FUN_xxxxxxxx` address with the signature implied
 * by the call site. All of this graph is gc-sectioned away in the stub
 * build (the state machine is unrooted until BIOS_start is decoded).
 *
 * OEM handler addresses are noted per function.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* HANDLED sentinel — must match SM_HANDLED in state_machine.c. */
#define SM_HANDLED  0x03000000u

/* Shared state-machine context block (RAM 0x20005950); defined in
 * state_machine.c. Handlers read a "suppress re-advertise" flag at byte +2
 * and three TI-RTOS clock handles at words +4 / +8 / +0xc (armed via
 * FUN_00025f8c). The block is volatile because the dispatcher and the
 * clock ISRs both touch it. */
extern volatile uint8_t g_sm_context[];

/* ---- Decoded callees in other TUs ---------------------------------- */
extern void audiotask_kick(int action);                   /* FUN_000237C8 */
extern void buttonpress_assert_invalid_lockstate(void);   /* FUN_0001E858 */
extern int  bond_store_dump_log(void);                    /* FUN_0000A07C */
extern void gap_adv_disable_set1(void);                   /* FUN_00024FE8 */
extern void gap_adv_disable_set2(void);                   /* FUN_00025010 */
extern void firmware_abort(void);                         /* FUN_0001F7F8 */
/* gap_adv_apply_set1 (0x12EB0), xs3_gatt_process_write_event (0x4DB0),
 * monitor_alloc (0x13470) are declared in bleware.h. */

/* ---- Undecoded leaf callees (signatures from the call sites) -------- */
extern void FUN_0000a458(void);
extern void FUN_0000a81c(void);
extern void FUN_00009274(void *payload);
extern void FUN_00014ee4(uint16_t v);
extern void FUN_00016f2c(void);
extern void FUN_0002168c(uint32_t conn, void *payload);
extern void FUN_0002200c(void *payload);
extern void FUN_00023800(int v);
extern void FUN_00024508(uint32_t char_uuid, uint8_t value);
extern void FUN_00025400(void);
extern int  FUN_00025a84(void);
extern void FUN_00025b04(int v);
extern void FUN_00025f8c(uint32_t clock_handle, int period_ms, void *arg);
extern int  FUN_0002309c(uint32_t conn, uint16_t value);
extern void FUN_000211f8(int conn, uint16_t a, uint16_t b, uint32_t c, uint16_t d);
extern int  FUN_0001f640(uint32_t conn, uint16_t *o1, uint16_t *o2, uint16_t *o3);
extern void FUN_000254b4(uint16_t v);
extern void FUN_000261b2(uint32_t clock_handle);
extern int  FUN_00026a7c(void);          /* connection-count query (validation: TBD) */
extern void FUN_00026a40(void *payload);
extern int  FUN_00026c54(void);          /* != 0 => a peer is connected */
extern void FUN_00026fc4(uint16_t v);
extern void FUN_00026ff4(void);
extern void FUN_00027004(void);
extern void FUN_000265c4(int v);
extern void FUN_000275a8(void);
extern void FUN_000275b0(void);

/* ===================================================================
 * State 0 — the single transition
 * =================================================================== */

/* event 0x00 — OEM 0x00025944. Kick the cold-start helper, then arm the
 * 1000 ms clock stored at g_sm_context + 8. */
uint32_t sm_handler_ev00(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_0000a458();
    FUN_00025f8c(*(volatile uint32_t *)(g_sm_context + 8), 1000, NULL);
    return SM_HANDLED;
}

/* ===================================================================
 * State 1 — the 25 transitions
 * =================================================================== */

/* event 0x01 — OEM 0x00027424. */
uint32_t sm_handler_ev01(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_0000a81c();
    return SM_HANDLED;
}

/* event 0x02 — OEM 0x00027164. */
uint32_t sm_handler_ev02(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_0002200c((void *)payload);
    return SM_HANDLED;
}

/* event 0x03 — OEM 0x00027172. */
uint32_t sm_handler_ev03(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00026a40((void *)payload);
    return SM_HANDLED;
}

/* event 0x04 — OEM 0x00027180. */
uint32_t sm_handler_ev04(uint32_t event_id, const void *payload, uint32_t len)
{
    buttonpress_assert_invalid_lockstate();
    return SM_HANDLED;
}

/* event 0x05 — OEM 0x00024B0C. Re-apply the primary advert set, but only
 * while there is room for a new link (< 3 connections), the suppress flag
 * at g_sm_context + 2 is clear, and no advert is already active. */
uint32_t sm_handler_ev05(uint32_t event_id, const void *payload, uint32_t len)
{
    if (FUN_00026a7c() < 3 && g_sm_context[2] != 1 && FUN_00025a84() < 1) {
        gap_adv_apply_set1();
    }
    return SM_HANDLED;
}

/* event 0x06 — OEM 0x000261E6. */
uint32_t sm_handler_ev06(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_000254b4(*(const uint16_t *)payload);
    if (FUN_00025a84() < 1) {
        gap_adv_apply_set1();
    }
    return SM_HANDLED;
}

/* event 0x0f — OEM 0x00026200. Run the central GATT write dispatcher; on a
 * negative result, signal the failure for the connection handle at +2. */
uint32_t sm_handler_ev0f(uint32_t event_id, const void *payload, uint32_t len)
{
    if (xs3_gatt_process_write_event((struct gatt_write_event *)payload) < 0) {
        FUN_00026fc4(*(const uint16_t *)((const uint8_t *)payload + 2));
    }
    return SM_HANDLED;
}

/* event 0x10 — OEM 0x00024CEA. */
uint32_t sm_handler_ev10(uint32_t event_id, const void *payload, uint32_t len)
{
    const uint8_t *p    = (const uint8_t *)payload;
    uint16_t       conn = *(const uint16_t *)(p + 2);
    uint8_t        b6   = p[6];
    short          val  = (short)(*(const short *)(p + 4) + b6 + 1);

    FUN_0002309c(conn, (uint16_t)val);
    FUN_0002168c(conn, (void *)payload);
    FUN_00025b04(val);
    return SM_HANDLED;
}

/* event 0x12 — OEM 0x00026C8A. */
uint32_t sm_handler_ev12(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00024508(0x10e, 0);
    return SM_HANDLED;
}

/* event 0x13 — OEM 0x00027400. */
uint32_t sm_handler_ev13(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00016f2c();
    return SM_HANDLED;
}

/* event 0x14 — OEM 0x00025C76. */
uint32_t sm_handler_ev14(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00024508(0x10e, 3);
    FUN_00024508(0x5571, *(const uint8_t *)payload);
    return SM_HANDLED;
}

/* event 0x15 — OEM 0x00026C9C. */
uint32_t sm_handler_ev15(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00024508(0x10e, 7);
    return SM_HANDLED;
}

/* event 0x16 — OEM 0x00026C66. */
uint32_t sm_handler_ev16(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00024508(0x10e, 5);
    return SM_HANDLED;
}

/* event 0x17 — OEM 0x00026C78. */
uint32_t sm_handler_ev17(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00024508(0x10e, 8);
    return SM_HANDLED;
}

/* event 0x18 — OEM 0x0001D7AC. Auth-state transition: allocate a 2-byte
 * event payload copy, either clear the auth grace path (connected) or
 * raise sub-status 0x1b (not connected), tear down the active advert, and
 * if the copy succeeded, stamp the peer's handle into it and (re)arm the
 * 7500 ms (0x1d4c) auth clock at g_sm_context + 0xc. The copy's ownership
 * passes to that clock's callback. */
uint32_t sm_handler_ev18(uint32_t event_id, const void *payload, uint32_t len)
{
    uint16_t *evt_copy = monitor_alloc(2);

    if (FUN_00026c54() == 0) {
        FUN_00027004();
        FUN_00025f8c(*(volatile uint32_t *)(g_sm_context + 4), 200, NULL);
    } else {
        FUN_000265c4(0x1b);
    }

    FUN_000275a8();
    gap_adv_disable_set2();
    FUN_00023800(0);
    if (FUN_00025a84() > 0) {
        gap_adv_disable_set1();
    }

    uint32_t auth_clock = *(volatile uint32_t *)(g_sm_context + 0xc);
    if (evt_copy != NULL) {
        *evt_copy = *(const uint16_t *)payload;
        FUN_000261b2(auth_clock);
        FUN_00025f8c(*(volatile uint32_t *)(g_sm_context + 0xc), 0x1d4c, evt_copy);
    }
    return SM_HANDLED;
}

/* event 0x19 — OEM 0x00026088. */
uint32_t sm_handler_ev19(uint32_t event_id, const void *payload, uint32_t len)
{
    if (FUN_00025a84() < 1) {
        FUN_00026ff4();
        FUN_000275b0();
        gap_adv_apply_set1();
    }
    return SM_HANDLED;
}

/* event 0x1a — OEM 0x00024D3E. Query the connection record for the handle
 * in the payload, then push a 200 ms param-update request for it. The low
 * 16 bits of `interval` are filled in by FUN_0001f640 (an out-parameter). */
uint32_t sm_handler_ev1a(uint32_t event_id, const void *payload, uint32_t len)
{
    uint16_t conn     = *(const uint16_t *)payload;
    uint32_t interval = 0;   /* low half written by FUN_0001f640; high half unused */

    FUN_0001f640(conn, (uint16_t *)&interval, NULL, NULL);
    FUN_000211f8(conn, 0, 200, interval & 0xffff, (uint16_t)(interval & 0xffff));
    return SM_HANDLED;
}

/* event 0x1b — OEM 0x000273E8. */
uint32_t sm_handler_ev1b(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_000275a8();
    return SM_HANDLED;
}

/* event 0x1c — OEM 0x000271C6. */
uint32_t sm_handler_ev1c(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00014ee4(*(const uint16_t *)payload);
    return SM_HANDLED;
}

/* event 0x1d — OEM 0x000271B8. */
uint32_t sm_handler_ev1d(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00009274((void *)payload);
    return SM_HANDLED;
}

/* event 0x1e — OEM 0x00026AB8. */
uint32_t sm_handler_ev1e(uint32_t event_id, const void *payload, uint32_t len)
{
    if (FUN_00025a84() < 1) {
        FUN_00026ff4();
    }
    return SM_HANDLED;
}

/* event 0x1f — OEM 0x00027156. Kick the audio task with the action byte in
 * the payload. (The OEM stores audiotask_kick's scratch return through a
 * dead pointer; the store has no observable effect and is elided.) */
uint32_t sm_handler_ev1f(uint32_t event_id, const void *payload, uint32_t len)
{
    audiotask_kick(*(const uint8_t *)payload);
    return SM_HANDLED;
}

/* event 0x20 — OEM 0x000273C4. */
uint32_t sm_handler_ev20(uint32_t event_id, const void *payload, uint32_t len)
{
    FUN_00025400();
    return SM_HANDLED;
}

/* event 0x23 — OEM 0x000273B8. */
uint32_t sm_handler_ev23(uint32_t event_id, const void *payload, uint32_t len)
{
    bond_store_dump_log();
    return SM_HANDLED;
}

/* event 0x24 — OEM 0x00027430. The OEM emits the firmware-abort call
 * followed by the usual HANDLED return (the compiler did not treat the
 * abort as noreturn here); reproduced verbatim. */
uint32_t sm_handler_ev24(uint32_t event_id, const void *payload, uint32_t len)
{
    firmware_abort();
    return SM_HANDLED;
}
