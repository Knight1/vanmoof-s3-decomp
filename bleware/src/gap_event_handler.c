/* gap_event_handler.c — GAP Host event dispatcher for ICall event
 * class 0x91 sub-code 0x3E, reached from the bluetoothtask event loop.
 *
 * OEM address: 0x00010b40 (290 B)
 *
 * This function receives a pre-built ICall message struct and dispatches
 * on the GAP Host opcode at msg[2].
 *
 * The message struct layout (offsets from param_1):
 *   +0x00: padding
 *   +0x01: reserved
 *   +0x02: GAP opcode (uint8_t)
 *   +0x03: sub-flags (uint8_t)
 *   +0x04: param0 (uint16_t) — often conn handle
 *   +0x06: param1 (uint16_t)
 *   +0x08: param2 (uint16_t)
 *   +0x0A: param3 (uint16_t)
 *   +0x0C: param4 (uint16_t)
 */

#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* External ROM thunks — TI CC2642R1F BLE5-Stack ROM (AGAMA R1),
 * resolved by ldr.w pc, [literal] indirect jumps. */

/* ROM 0x10019188 — GAP_EstablishLink handler.
 * Reached via the ldr.w pc,[lit] tail-jump thunk at 0x00027cb0; the ROM
 * function returns normally to the caller (lr from the bl), so this is NOT
 * a noreturn. */
void rom_gap_establish_link(uint8_t opcode, const void *msg);

/* ROM 0x10021D10 — GAP_UpdateLinkParamReq handler, returns status */
int  rom_gap_update_link_param(uint16_t conn, uint8_t opcode, const void *msg);

/* ROM 0x10025464 — Config connection params */
void rom_gap_config_conn_params(uint16_t min_interval, uint16_t max_interval,
                                uint16_t latency,   uint16_t timeout);

/* ROM 0x10022C94 — GAP set param */
void rom_gap_set_param(uint16_t conn, uint16_t param_id, uint16_t value);

/* ROM 0x1001BD20 — GAP get param, returns status byte (0/1/2) */
uint8_t rom_gap_get_param(uint32_t param_id);

/* ROM 0x100157D8 — GAP send a pre-built msg on the stack */
void rom_gap_send_msg(const void *stack_msg);

/* Sub-dispatcher stub — OEM at 0x000276b2 and 0x000276c2.
 * In the real build these are resolved by TI linker config; here
 * they're weak no-ops returning 1. */
int  gap_event_sub_dispatch(uint8_t opcode, uint32_t msg_len, const void *msg);

/* Queue post helper — OEM at 0x0000e5a8. Posts a built message to
 * the service queue identified by the byte at *DAT_00010c64
 * (RAM 0x2000566c — the destination service ID). */
void queue_post_message(uint8_t service_id, void *msg);

/* Destination service ID (byte at RAM 0x2000566c). */
extern uint8_t s_dest_service_id;

/*
 * GAP Host command dispatcher for ICall event 0x91 sub-code 0x3E.
 *
 * Parameter `msg` is r0 — a pointer to the ICall message struct.
 * Returns a status code in r0 (1 = ok/sent, 0 = unknown opcode).
 */
uint32_t gap_event_91_3e_handler(const void *msg)
{
    const uint8_t  *m = (const uint8_t *)msg;
    uint8_t         opcode;
    uint32_t        result;     /* r4 — return accumulator */
    uint16_t        conn;
    uint8_t         gap_status; /* r0 after rom_gap_get_param */
    void           *alloc_buf;
    uint8_t         stack_msg[12];  /* local_18..local_10 on the stack */

    result = 1;
    opcode = m[2];

    switch (opcode) {
    /* Opcodes 0x01 and 0x0A — establish a BLE link.
     * OEM @ 0x00010b7e: bl 0x00027cb0 (GAP_EstablishLink tail-jump) then
     * b 0x00010c5e (return r4 = 1). The ROM call returns; the handler's
     * return value stays 1 (set before the switch). No fall-through. */
    case 1:
    case 0x0A:
        rom_gap_establish_link(opcode, msg);
        result = 1;
        break;

    /* Opcode 0x03 and 0x83 — update link parameters */
    case 3:
    case 0x83:
        result = rom_gap_update_link_param(*(const uint16_t *)(m + 4),
                                           opcode, msg);
        if (m[3] == 0) {
            rom_gap_config_conn_params(*(const uint16_t *)(m + 4),
                                       *(const uint16_t *)(m + 6),
                                       *(const uint16_t *)(m + 10),
                                       *(const uint16_t *)(m + 8));
        }
        break;

    /* Opcode 0x06 — terminate a BLE link */
    case 0x06:
        conn = *(const uint16_t *)(m + 4);
        rom_gap_set_param(conn, 0x40, 1);
        gap_status = rom_gap_get_param(0);

        if (gap_status == 2) {
            alloc_buf = monitor_alloc(0x10);
            if (alloc_buf == NULL) {
                rom_gap_set_param(conn, 0x40, 0);
            } else {
                ((uint8_t *)alloc_buf)[0]  = 0xD0;
                ((uint8_t *)alloc_buf)[1]  = m[3];
                ((uint8_t *)alloc_buf)[2]  = 0x11;
                *(uint16_t *)((uint8_t *)alloc_buf + 4)  = *(const uint16_t *)(m + 4);
                *(uint16_t *)((uint8_t *)alloc_buf + 6)  = *(const uint16_t *)(m + 6);
                *(uint16_t *)((uint8_t *)alloc_buf + 8)  = *(const uint16_t *)(m + 8);
                *(uint16_t *)((uint8_t *)alloc_buf + 10) = *(const uint16_t *)(m + 10);
                *(uint16_t *)((uint8_t *)alloc_buf + 12) = *(const uint16_t *)(m + 12);
                ((uint8_t *)alloc_buf)[14] = 0;
                queue_post_message(s_dest_service_id, alloc_buf);
            }
        } else {
            /* Build a 12-byte gap-end-msg on the stack and send it.
             * The struct layout:
             *   +0  u16  conn_handle (or min_interval depending on status)
             *   +2  u16  max_interval (only if status==0)
             *   +4  u16  latency     (only if status==0)
             *   +6  u16  timeout     (only if status==0)
             *   +8  u16  (from msg+12) (only if status==0)
             *   +10 u8   0 (only if status==0)
             *   +11 u8   flag: 0 for status==1, 1 for status==0
             */
            *(uint16_t *)(stack_msg + 0) = conn;
            if (gap_status == 1) {
                stack_msg[11] = 0;
            } else {
                /* gap_status == 0 */
                *(uint16_t *)(stack_msg + 0) = *(const uint16_t *)(m + 4);
                *(uint16_t *)(stack_msg + 2) = *(const uint16_t *)(m + 6);
                *(uint16_t *)(stack_msg + 4) = *(const uint16_t *)(m + 8);
                *(uint16_t *)(stack_msg + 6) = *(const uint16_t *)(m + 10);
                *(uint16_t *)(stack_msg + 8) = *(const uint16_t *)(m + 12);
                stack_msg[10] = 0;
                stack_msg[11] = 1;
            }
            rom_gap_send_msg(stack_msg);
        }
        break;

    /* Opcodes 0x0E, 0x0F, 0x10 — sub-dispatcher, no length arg */
    case 0x0E:
    case 0x0F:
    case 0x10:
        result = gap_event_sub_dispatch(opcode, 0, msg);
        break;

    /* Opcodes 0x15, 0x16, 0x81 — sub-dispatcher with 0x18 byte length */
    case 0x15:
    case 0x16:
    case 0x81:
        result = gap_event_sub_dispatch(opcode, 0x18, msg);
        break;

    /* Opcode 0x17 — sub-dispatcher with 6 byte length */
    case 0x17:
        result = gap_event_sub_dispatch(opcode, 6, msg);
        break;

    /* Opcode 0x84 — sub-dispatcher with 0x1C byte length */
    case 0x84:
        result = gap_event_sub_dispatch(opcode, 0x1C, msg);
        break;

    /* Unknown opcode — return 0 (no action taken) */
    default:
        result = 0;
        break;
    }

    return result;
}
