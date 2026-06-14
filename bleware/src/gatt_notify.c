/* gatt_notify.c — per-channel GATT notification dispatcher.
 *
 * 5 notify channels (0..4), each with RAM buffer and connection handle.
 * Copies payload, then if conn_handle is active, sends GATT_Notification
 * via the TI BLE-stack (FUN_00016D1C).
 *
 * OEM @ 0x0001B538 (98 B).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Per-channel state struct at RAM 0x200049CC (DAT_0001B5B0):
 *   -4: u8 tag  (0x0E = notification opcode for GATT_Notification)
 *   +4: u32 ch2_conn_handle
 *   +8: u32 ch0_conn_handle
 *   +12: u32 ch4_conn_handle
 *   +16: void *extra (GATT param block pointer) */
extern uint8_t g_gatt_notify_chan_tag;       /* at state_base - 4 */

extern void *memcpy(void *dst, const void *src, unsigned int n);
extern int   FUN_00016D1C(int conn, void *buf, uint32_t flag,
                          void *param, uint16_t len, uint8_t tag,
                          uint32_t extra);

/* Per-channel buffer globals (OEM literal pool):
 * ch 0: 0x2000ACE0 (16 B, svc 0x5501), ch 2: 0x2000ACC0 (16 B, svc 0x5503)
 * ch 3: 0x2000ACD0 (16 B, svc 0x5504), ch 4: 0x20009F24 (0xF0 B, svc 0x5505)
 * All are the VALUE at the DAT_ literal, not a pointer-to-pointer. */
#define CH0_BUF  ((void *)0x2000ACE0u)
#define CH2_BUF  ((void *)0x2000ACC0u)
#define CH3_BUF  ((void *)0x2000ACD0u)
#define CH4_BUF  ((void *)0x20009F24u)

int gatt_notify_channel(int channel, const void *buf)
{
    extern uint32_t g_gatt_notify_state[5];  /* +0:pad, +4:ch2_conn, +8:ch0_conn,
                                                +12:ch4_conn, +16:param_blk */
    void    *chan_buf;
    uint16_t copy_len;
    int      conn_handle;
    int      do_notify = 1;

    switch (channel) {
    case 0:
        chan_buf    = CH0_BUF;
        conn_handle = (int)g_gatt_notify_state[2];  /* +8 */
        do_notify   = 0;
        copy_len    = 16;
        break;
    case 2:
        chan_buf    = CH2_BUF;
        conn_handle = (int)g_gatt_notify_state[1];  /* +4 */
        copy_len    = 16;
        break;
    case 3:
        chan_buf    = CH3_BUF;
        conn_handle = (int)g_gatt_notify_state[2];  /* +8 (same handle as ch0) */
        copy_len    = 16;
        break;
    case 4:
        chan_buf    = CH4_BUF;
        conn_handle = (int)g_gatt_notify_state[3];  /* +12 */
        copy_len    = 0xF0;
        break;
    default:
        return 2;
    }

    memcpy(chan_buf, buf, copy_len);

    if (do_notify && conn_handle != 0) {
        uint8_t tag = g_gatt_notify_chan_tag;
        /* OEM passes: 4th = ADDRESS of the +0x10 param block; 5th = fixed
         * length 0xE (not copy_len); 7th = fixed literal 0x0001E181. */
        FUN_00016D1C(conn_handle, chan_buf, 0,
                     (void *)&g_gatt_notify_state[4],  /* +0x10 param block, by address */
                     0xEu, tag,
                     0x0001E181u);
    }
    return 0;
}
