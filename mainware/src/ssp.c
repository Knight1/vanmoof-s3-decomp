#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crc.h"
#include "log.h"
#include "panic.h"
#include "scheduler.h"
#include "ssp.h"
#include "systick.h"
#include "uart.h"
#include "util.h"

/* --- externs supplied elsewhere in the image ---
 * ble_cmd_dispatch           0x08033970  BLE command dispatcher (write side).
 * ble_read_request_dispatch  0x08034D20  GATT/SSP read-request dispatcher.
 * The TX heap payloads are malloc'd (0x08020E40) and freed (0x08020E50) via the
 * image's newlib allocator. */
extern void ble_cmd_dispatch(uint32_t cmd, uint32_t p2, uint8_t *payload);
extern void ble_read_request_dispatch(uint16_t char_id);

/* RX/TX primitives defined at the end of this file. */
int ssp_rx_byte(uint8_t *out);                /* OEM 0x08036528 */
int ssp_ble_seq_id_in_use(uint8_t seq);       /* OEM 0x0803F470 — scan tx_queue for a seq id */

/* SLIP/BLE message handlers, defined at the end of this file. */
static void ble_data_packet(uint16_t cmd, uint16_t len, uint8_t *data, uint8_t len_hi);
static void ble_prepare_packet(uint16_t char_id);
static int  ble_command(uint8_t id);

/* TX side. */
extern uint8_t g_ssp_tx_seq;   /* rolling TX sequence id (OEM 0x200000F0 + 0x10) */

/* ── outbound-pump dependencies (sourced in their home modules) ── */
extern void    state_flags_set(uint32_t set_mask, uint32_t clear_mask);
extern uint8_t maybe_get_bike_state(void);
extern void    update_mode_request(uint8_t mode);
extern void    shifter_mode_command_dispatch(uint8_t cmd);
extern void    HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);

/* Inter-module (SSPM) bus TX primitives — defined at the end of this file. */
int      sspm_bus_send_byte(uint8_t b);                       /* OEM 0x0803662C */
uint32_t sspm_bus_send_frame(const uint8_t *buf, uint16_t len); /* OEM 0x0803A008 */

/* One outbound packet descriptor (12 bytes); type == 5 means the slot is in use. */
typedef struct {
    uint8_t  flags;    /* +0x00 */
    uint8_t  type;     /* +0x01  5 = occupied, 0 = free */
    uint8_t  _resv;    /* +0x02 */
    uint8_t  seq;      /* +0x03  unique sequence id */
    uint16_t cmd;      /* +0x04 */
    uint16_t len;      /* +0x06 */
    void    *payload;  /* +0x08  heap copy of the payload */
} ssp_tx_entry_t;

/* The BLE/SSP context (OEM SRAM 0x20008A40). The 128-entry TX queue occupies
 * the first 0x600 bytes; the SLIP de-framer state and the ack frame follow it. */
struct ble_ssp_ctx {
    ssp_tx_entry_t tx_queue[128]; /* +0x000..+0x5FF (128 x 12 B) */
    uint8_t slip_state;      /* +0x600 */
    uint8_t got_packet;      /* +0x601 — cleared when a frame is consumed */
    uint8_t _pad1[0x10E];    /* +0x602..+0x70F */
    uint8_t response[6];     /* +0x710 — {1, 5, id, ...} ack frame */
};
extern struct ble_ssp_ctx g_ble_ssp;        /* SRAM 0x20008A40 */
extern struct slip_rx      g_slip_rx;        /* SRAM 0x200000F4 */
extern uint8_t            *g_ble_rx_msg;     /* de-framed message buffer (OEM *(0x200000F0+4)) */

char slip_rx_packet(struct slip_rx *ctrl)
{
    uint8_t b;

    if (ssp_rx_byte(&b) == 0) {
        return 1;                 /* no byte yet — still receiving */
    }

    char ret = (char)g_ble_ssp.slip_state;

    switch (g_ble_ssp.slip_state) {
    case SLIP_IN_FRAME:
        if (b == SLIP_END) {
            if (ctrl->len != 0) {
                /* Modbus CRC-16 residue 0 over data+trailer == valid. */
                if (crc16(ctrl->buf, (int)ctrl->len, 0xFFFF) == 0) {
                    ret = 0;
                } else {
                    g_log_func("ERR BLE-CRC\r\n");
                    ret = 2;
                }
                g_ble_ssp.slip_state = SLIP_IDLE;
            }
        } else if (b == SLIP_ESC) {
            g_ble_ssp.slip_state = SLIP_AFTER_ESC;
        } else if (ctrl->len < ctrl->cap) {
            ctrl->buf[ctrl->len++] = b;
        }
        break;

    case SLIP_AFTER_ESC:
        if (b == SLIP_ESC_END) {
            if (ctrl->len < ctrl->cap) ctrl->buf[ctrl->len++] = SLIP_END;
            ret = 1;
            g_ble_ssp.slip_state = SLIP_IN_FRAME;
        } else if (b == SLIP_ESC_ESC) {
            if (ctrl->len < ctrl->cap) ctrl->buf[ctrl->len++] = SLIP_ESC;
            ret = 1;
            g_ble_ssp.slip_state = SLIP_IN_FRAME;
        } else {
            g_log_func("BLE-SEQ\r\n");        /* bad escape sequence */
            g_ble_ssp.slip_state = SLIP_IDLE;
        }
        break;

    case SLIP_IDLE:
        if (b == SLIP_END) {
            ctrl->len = 0;
            ret = 1;
            g_ble_ssp.slip_state = SLIP_IN_FRAME;
        } else {
            ret = 1;
        }
        break;

    default:
        ret = 1;
        break;
    }

    return ret;
}

int ble_ssp_dispatch(void)
{
    char r = slip_rx_packet(&g_slip_rx);

    if (r == 0) {
        g_ble_ssp.got_packet = 0;
        uint8_t *msg = g_ble_rx_msg;
        uint8_t type = msg[1];
        uint8_t id   = msg[2];

        if (type == BLE_MSG_DATA) {                  /* 0x07 — command + payload */
            ble_data_packet((uint16_t)(msg[3] | (msg[4] << 8)),
                            (uint16_t)(msg[5] | (msg[6] << 8)),
                            &msg[7], msg[6]);
            g_ble_ssp.response[0] = 1;
            g_ble_ssp.response[1] = 5;
            g_ble_ssp.response[2] = id;
            slip_send_frame(g_ble_ssp.response, 3);
        } else if (type == BLE_MSG_PREPARE) {        /* 0x06 — length announce */
            ble_prepare_packet((uint16_t)(msg[3] | (msg[4] << 8)));
            g_ble_ssp.response[0] = 1;
            g_ble_ssp.response[1] = 5;
            g_ble_ssp.response[2] = id;
            slip_send_frame(g_ble_ssp.response, 3);
        } else if (type == BLE_MSG_COMMAND) {        /* 0x05 — control command */
            if (ble_command(id) == 0) {
                g_log_func("ERR BLE SSP packet not in queue\r\n");
            }
        }
    } else if (r == 2) {
        g_log_func("SSPB FAILED\r\n");
    }

    return r == 0;
}

/* Enqueue an outbound packet into the 128-slot TX queue (OEM
 * ssp_ble_enqueue_tx_packet, 0x0803F9CC). Finds a free slot, assigns a unique
 * sequence id (skipping ids still queued — re-testing the same slot on a
 * collision, per the OEM), heap-copies the payload, and fills the descriptor.
 * Returns the slot index 0..0x7F, 0xFD if len > 0x100, or 0xFF if full. */
uint8_t ssp_ble_enqueue_tx_packet(uint16_t cmd, uint16_t len,
                                  const void *payload, uint8_t flags)
{
    uint32_t i;

    if (len > 0x100u) {
        return 0xFD;
    }

    for (i = 0; (i & 0x80u) == 0; ) {
        if (g_ble_ssp.tx_queue[i].type != 0) {        /* slot occupied -> next slot */
            i = (i + 1) & 0xFFu;
            continue;
        }

        uint8_t seq = g_ssp_tx_seq;
        if (ssp_ble_seq_id_in_use(seq) != 0) {        /* seq in use -> bump, retest slot */
            g_ssp_tx_seq = (uint8_t)(seq + 1);
            continue;
        }
        g_ssp_tx_seq = (uint8_t)(seq + 1);

        void *dst = malloc(len);
        if (dst == 0) {
            muco_assert_fail("src/ssp_ble.c", 0x22B);  /* noreturn */
        }
        memcpy(dst, payload, len);

        g_ble_ssp.tx_queue[i].flags   = flags;
        g_ble_ssp.tx_queue[i].type    = 5;
        g_ble_ssp.tx_queue[i].seq     = seq;
        g_ble_ssp.tx_queue[i].cmd     = cmd;
        g_ble_ssp.tx_queue[i].len     = len;
        g_ble_ssp.tx_queue[i].payload = dst;
        return (uint8_t)i;
    }
    return 0xFF;   /* queue full */
}

/* --- SLIP/BLE message handlers (dispatched by ble_ssp_dispatch) --- */

/* Type-0x07 data message → BLE command dispatcher (OEM ble_data_packet
 * 0x0803F6A4, a thunk forwarding cmd/len/payload to ble_cmd_dispatch). */
static void ble_data_packet(uint16_t cmd, uint16_t len, uint8_t *data, uint8_t len_hi)
{
    (void)len_hi;
    ble_cmd_dispatch(cmd, len, data);
}

/* Type-0x06 read/prepare message → GATT read dispatcher (OEM ble_prepare_packet
 * 0x0803F6AC, a thunk). */
static void ble_prepare_packet(uint16_t char_id)
{
    ble_read_request_dispatch(char_id);
}

/* Type-0x05 control message: an ACK that releases the queued TX packet whose
 * sequence id matches and frees its heap payload (OEM ble_command, 0x0803F498).
 * Returns 1 if a matching live slot was found, else 0. */
static int ble_command(uint8_t id)
{
    uint32_t i;

    for (i = 0; (i & 0x80u) == 0; i = (i + 1) & 0xFFu) {
        ssp_tx_entry_t *e = &g_ble_ssp.tx_queue[i];
        if (e->seq == id && e->type != 0) {
            e->type = 0;            /* release the slot */
            free(e->payload);       /* free the heap copy of the payload */
            return 1;
        }
    }
    return 0;
}

/* SLIP byte emit with escaping (helper for slip_send_frame). */
static void slip_put(uint8_t b)
{
    if (b == SLIP_END) {
        uart_send_byte(SLIP_ESC);
        uart_send_byte(SLIP_ESC_END);
    } else if (b == SLIP_ESC) {
        uart_send_byte(SLIP_ESC);
        uart_send_byte(SLIP_ESC_ESC);
    } else {
        uart_send_byte(b);
    }
}

/* SLIP-frame + transmit a payload with a CRC-16 trailer (OEM slip_send_frame,
 * 0x0803F4F0 — the TX counterpart of slip_rx_packet). Wire format:
 * 0xC0 [escaped payload] [escaped CRC-lo] [escaped CRC-hi] 0xC0. */
uint32_t slip_send_frame(const uint8_t *buf, int len)
{
    uint16_t crc = crc16(buf, len, 0xFFFFu);

    uart_send_byte(SLIP_END);
    while (len != 0) {
        slip_put(*buf);
        buf++;
        len--;
    }
    slip_put((uint8_t)(crc & 0xFF));
    slip_put((uint8_t)((crc >> 8) & 0xFF));

    /* only the closing delimiter's TX status is checked by the OEM */
    if (uart_send_byte(SLIP_END) == 0) {
        return 2;
    }
    return 0;
}

/* Atomically pop one byte from the bus RX ring (OEM ssp_rx_byte, 0x08036528).
 * The RX-source interrupt is masked around the ring access:
 *   g_ssp_rx_dev_pp @ 0x20009864 -> a wrapper whose first word points at the
 *   peripheral control block; that block's +0xC word is the interrupt-enable,
 *   bit 5 (0x20) gates the RX source. The RX ring handle is at *(g_ssp_ctx+0xB3C)
 *   (g_ssp_ctx @ 0x20001A44 — the same UART/bus context uart_send_byte uses).
 * ABI quirk preserved (as with uart_send_byte): the function returns no value of
 * its own — r0 survives from ringbuf_get_byte through the trailing unmask — so it
 * implicitly returns the get status (1 = byte produced, 0 = empty). slip_rx_packet
 * relies on this. */
int ssp_rx_byte(uint8_t *out)
{
    void * volatile *pp_wrap = (void * volatile *)0x20009864u;
    void *wrap = *pp_wrap;                                  /* control wrapper object */
    volatile uint32_t *ctl = *(volatile uint32_t * volatile *)wrap;  /* peripheral block */

    ctl[3] &= ~0x20u;                                       /* mask RX interrupt (reg +0xC) */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    ringbuf_t *rb = *(ringbuf_t * volatile *)(*(uint32_t *)0x20001A44u + 0xB40u);
    uint32_t rc = ringbuf_get_byte(rb, out);

    ctl = *(volatile uint32_t * volatile *)wrap;            /* OEM re-derefs the wrapper */
    ctl[3] |= 0x20u;                                        /* unmask RX interrupt */
    return (int)rc;
}

/* Scan all 128 TX-queue slots for one already using `seq` (OEM ssp_ble_seq_id_in_use,
 * 0x0803F470). The OEM compares the seq byte (+3) of every slot regardless of
 * occupancy. Returns 1 if found, else 0. */
int ssp_ble_seq_id_in_use(uint8_t seq)
{
    for (int i = 0; i < 128; i++) {
        if (g_ble_ssp.tx_queue[i].seq == seq) {
            return 1;
        }
    }
    return 0;
}

/* ── generic outbound-message table (distinct from the BLE/SSP tx_queue) ─────
 * A 16-slot x 24-byte table at SRAM 0x20007E14, drained by sspm_tx_queue_pump
 * (0x0803A278) and shared by the battery/motor/BLE/telemetry/console subsystems
 * for inter-module messages. */
typedef struct {
    uint8_t  type;        /* [0]  message type/opcode (param `type`)        */
    uint8_t  state;       /* [1]  slot state: 0 = free, 2 = pending         */
    uint8_t  _resv;       /* [2]  committed from uninitialized stack         */
    uint8_t  handle;      /* [3]  rolling unique handle                      */
    uint16_t id;          /* [4]  message id / arg (param `id`)             */
    uint16_t len;         /* [6]  payload length in bytes                    */
    uint8_t  payload[16]; /* [8]  payload bytes                              */
} tx_msg_record_t;

#define TX_MSG_TABLE       ((volatile tx_msg_record_t *)0x20007e14u)
#define TX_MSG_SLOTS       16
#define TX_MSG_HANDLE_CTR  (*(volatile uint8_t *)0x200000c8u)

extern uint8_t update_mode_get(void);              /* 0x080313d8: link state, 2 = ready */

/* Scan the 16-slot table for a rolling handle (OEM 0x08039fe0). The OEM compares
 * the handle byte (+3) of every slot regardless of occupancy; returns 1 on the
 * first match, else 0. Used by maybe_enqueue_tx_message to avoid a duplicate handle. */
int tx_table_handle_in_use(uint8_t h)
{
    for (int i = 0; i < TX_MSG_SLOTS; i++) {
        if (TX_MSG_TABLE[i].handle == h) {
            return 1;
        }
    }
    return 0;
}

/* Enqueue one outbound message into the table, gated on the inter-module link
 * being connected (OEM maybe_enqueue_tx_message, 0x0803a1c4). Picks a free slot
 * (state == 0) and a non-colliding rolling handle from the 0x200000c8 counter;
 * on a handle collision it bumps the counter and advances to the next slot
 * (NOT a same-slot retry). Returns the slot index 0..15, 0 if not connected,
 * 0xFD if len > 16, 0xFF if the table is full. */
unsigned int maybe_enqueue_tx_message(uint16_t id, uint32_t len,
                                      const void *payload, uint8_t type)
{
    if (update_mode_get() != 2) {
        return 0;
    }
    if (len > 0x10u) {
        return 0xFD;
    }

    for (unsigned int slot = 0; slot < TX_MSG_SLOTS; slot++) {
        if (TX_MSG_TABLE[slot].state != 0) {
            continue;                       /* slot in use */
        }

        uint8_t handle = TX_MSG_HANDLE_CTR;
        if (tx_table_handle_in_use(handle) != 0) {
            TX_MSG_HANDLE_CTR = (uint8_t)(handle + 1);
            continue;                       /* handle collision -> next slot */
        }

        tx_msg_record_t rec;
        rec.type   = type;
        rec.state  = 2;                     /* pending */
        rec.handle = handle;
        rec.id     = id;
        rec.len    = (uint16_t)len;
        if (len != 0) {
            memcpy(rec.payload, payload, len);
        }

        TX_MSG_HANDLE_CTR = (uint8_t)(handle + 1);
        TX_MSG_TABLE[slot] = rec;
        return slot;
    }

    return 0xFF;                            /* table full */
}

/*
 * sspm_tx_queue_pump @ 0x0803A278 — inter-module-bus (SSP) outbound pump. Round-
 * robin drains the 16-entry x 0x18 outbound message table (g_msg_tx_table @
 * 0x20007E14, addressed here as flat bytes). Each slot: +0 type (1 => type-A
 * short frame, else type-B w/ payload), +1 state (2 = fresh, 0 = empty, else
 * retry countdown), +3 handle, +4 u16 arg, +6 u16 len, +8 payload. Trailing:
 * +0x181 committed counter (>4 trips the backlog path), +0x182 scan index (wraps
 * at 0x10), +0x184 frame staging buffer. Two scheduler timer slots @ 0x200000C8
 * pace it: [1] "retry_tmr" (0x32), [2] "between_pack_tmr" (0xF); a frame is only
 * emitted while BOTH are idle. Returns 1 when a slot's countdown reached 0.
 */
#define SSPM_TX_TABLE   ((uint8_t *)0x20007E14u)         /* == TX_MSG_TABLE, flat */
#define SSPM_TX_FRAME   ((uint8_t *)0x20007F98u)         /* == SSPM_TX_TABLE + 0x184 */
#define SSPM_TX_TIMERS  ((uint8_t *)0x200000C8u)         /* [1]=retry [2]=between */

#define SSPM_TBL_COMMITTED  0x181
#define SSPM_TBL_SCANIDX    0x182
#define SSPM_TBL_STRIDE     0x18
#define SSPM_RETRY_TICKS    0x32
#define SSPM_BETWEEN_TICKS  0x0F

static const char SSPM_RETRY_TMR_NAME[]   = "retry_tmr";
static const char SSPM_BETWEEN_TMR_NAME[] = "between_pack_tmr";

uint8_t sspm_tx_queue_pump(void)
{
    uint8_t *tbl    = SSPM_TX_TABLE;
    uint8_t *timers = SSPM_TX_TIMERS;
    uint8_t  idx;
    uint8_t *slot;
    uint8_t  state;

    /* Backlog overflow: too many committed frames unacknowledged. */
    if (tbl[SSPM_TBL_COMMITTED] > 4) {
        state_flags_set(0x400000u, 0u);
        if (maybe_get_bike_state() == 0x0Eu) {
            update_mode_request(0x03u);
        }
    }

    /* Lazily allocate the two scheduler timer slots (0xFA == SCHED_SLOT_NONE). */
    if (timers[1] == 0xFAu) {
        timers[1] = scheduler_alloc();
        scheduler_set_timer_name(timers[1], SSPM_RETRY_TICKS, SSPM_RETRY_TMR_NAME);
        scheduler_start(timers[1], SSPM_RETRY_TICKS, (sched_cb_t)0);
    }
    if (timers[2] == 0xFAu) {
        timers[2] = scheduler_alloc();
        scheduler_set_timer_name(timers[2], SSPM_BETWEEN_TICKS, SSPM_BETWEEN_TMR_NAME);
        scheduler_start(timers[2], SSPM_BETWEEN_TICKS, (sched_cb_t)0);
    }

    /* Advance round-robin scan index (wrap at 0x10). */
    idx = (uint8_t)(tbl[SSPM_TBL_SCANIDX] + 1);
    tbl[SSPM_TBL_SCANIDX] = idx;
    if (idx > 0x0Fu) {
        tbl[SSPM_TBL_SCANIDX] = 0;
    }

    idx  = tbl[SSPM_TBL_SCANIDX];
    slot = tbl + (uint32_t)idx * SSPM_TBL_STRIDE;

    state = slot[1];
    if (state != 0x02u) {
        if (state == 0x00u) {
            return 0;                                   /* empty slot */
        }
        if (scheduler_slot_is_idle(timers[1]) == 0) {   /* mid-retry: wait */
            return 0;
        }
    }

    if (scheduler_slot_is_idle(timers[2]) == 0) {       /* inter-packet pacing */
        return 0;
    }

    /* Both timers idle => emit. Restart both. */
    scheduler_start(timers[1], SSPM_RETRY_TICKS,   (sched_cb_t)0);
    scheduler_start(timers[2], SSPM_BETWEEN_TICKS, (sched_cb_t)0);

    idx  = tbl[SSPM_TBL_SCANIDX];
    slot = tbl + (uint32_t)idx * SSPM_TBL_STRIDE;

    if (slot[1] != 0) {
        slot[1] = (uint8_t)(slot[1] - 1);
    }

    if (slot[1] == 0) {
        tbl[SSPM_TBL_COMMITTED] = (uint8_t)(tbl[SSPM_TBL_COMMITTED] + 1);
        return 1;
    }

    /* Build the bus frame into the staging buffer (tbl + 0x184). */
    tbl[0x184] = 1;
    if (slot[0] == 0x01u) {
        /* type-A: opcode 6, handle + u16 arg, no payload. frame_len = 5. */
        tbl[0x185] = 6;
        tbl[0x186] = slot[3];
        {
            uint16_t arg = *(uint16_t *)(slot + 4);
            tbl[0x187] = (uint8_t)arg;
            tbl[0x188] = (uint8_t)(arg >> 8);
        }
        sspm_bus_send_frame(SSPM_TX_FRAME, 5);
    } else {
        /* type-B: opcode 7, handle + u16 arg + u16 len + payload. */
        uint16_t arg;
        uint16_t len;
        tbl[0x185] = 7;
        tbl[0x186] = slot[3];
        arg = *(uint16_t *)(slot + 4);
        tbl[0x187] = (uint8_t)arg;
        tbl[0x188] = (uint8_t)(arg >> 8);
        len = *(uint16_t *)(slot + 6);
        tbl[0x189] = (uint8_t)len;
        tbl[0x18a] = (uint8_t)(len >> 8);
        memcpy(tbl + 0x18b, slot + 8, len);
        sspm_bus_send_frame(SSPM_TX_FRAME, (uint16_t)(len + 7));
    }
    return 0;
}

/*
 * ssp_ble_tx_queue_pump @ 0x0803F6B4 — BLE-side outbound SLIP pump. Drains the
 * 128-entry transmit queue @ 0x20008A40 (0xC-byte slots; control bytes +0x601
 * stall counter, +0x602 scan index, +0x604.. SLIP frame staging) toward the BLE
 * co-processor, pacing with two timers in the pair @ 0x200000F0 ([0]=retry 100t,
 * [1]=packet 1t) and rebooting the BLE module (GPIOC pin5 pulse) if the queue
 * stalls (>2 unacked pumps). Payload pointers are malloc'd and free()d on send.
 * Returns 2 when a slot is consumed / blocked-state, else 0.
 */
#define g_ssp_ble_tx_timers ((uint8_t *)0x200000F0u)   /* [0]=retry_tmr [1]=packet_tmr */
#define g_ssp_ble_tx_queue  ((uint8_t *)0x20008A40u)   /* 128 x 0xC + control + frame */

uint8_t ssp_ble_tx_queue_pump(void)
{
    /* Lazily allocate the two pacing timers on first run. */
    if (g_ssp_ble_tx_timers[0] == SCHED_SLOT_NONE) {
        g_ssp_ble_tx_timers[0] = scheduler_alloc();
        scheduler_set_timer_name(g_ssp_ble_tx_timers[0], 100, "retry_tmr");
        scheduler_start(g_ssp_ble_tx_timers[0], 100, (sched_cb_t)0);
    }
    if (g_ssp_ble_tx_timers[1] == SCHED_SLOT_NONE) {
        g_ssp_ble_tx_timers[1] = scheduler_alloc();
        scheduler_set_timer_name(g_ssp_ble_tx_timers[1], 1, "packet_tmr");
        scheduler_start(g_ssp_ble_tx_timers[1], 1, (sched_cb_t)0);
    }

    /* Queue stalled (>2 unacked pumps): reset counter and recover. */
    if (g_ssp_ble_tx_queue[0x601] > 2) {
        g_ssp_ble_tx_queue[0x601] = 0;
        if (maybe_get_bike_state() == 0x1A) {
            shifter_mode_command_dispatch(4);
        } else if (maybe_get_bike_state() == 0x08 ||
                   maybe_get_bike_state() == 0x09) {
            return 2;
        } else {
            state_flags_set(0x800000, 0);
            g_log_func("Reboot BLE\r\n");
            HAL_GPIO_WritePin((void *)0x40021000, 0x20, 1);   /* GPIOC pin5 reset */
            systick_delay(10);
            HAL_GPIO_WritePin((void *)0x40021000, 0x20, 0);
        }
    }

    /* Advance the scan index, wrapping 0..127 (bit7 set => wrap). */
    g_ssp_ble_tx_queue[0x602] = (uint8_t)(g_ssp_ble_tx_queue[0x602] + 1);
    if ((g_ssp_ble_tx_queue[0x602] & 0x80) != 0) {
        g_ssp_ble_tx_queue[0x602] = 0;
    }

    {
        uint8_t  idx  = g_ssp_ble_tx_queue[0x602];
        uint8_t *slot = &g_ssp_ble_tx_queue[idx * 0xC];

        if (slot[1] != 5) {                 /* not freshly queued */
            if (slot[1] == 0) {             /* empty slot */
                return 0;
            }
            if (scheduler_slot_is_idle(g_ssp_ble_tx_timers[0]) == 0) {
                return 0;
            }
        }

        if (scheduler_slot_is_idle(g_ssp_ble_tx_timers[1]) == 0) {
            return 0;
        }
        scheduler_start(g_ssp_ble_tx_timers[1], 1, (sched_cb_t)0);
        scheduler_start(g_ssp_ble_tx_timers[0], 100, (sched_cb_t)0);

        idx  = g_ssp_ble_tx_queue[0x602];   /* OEM re-reads here */
        slot = &g_ssp_ble_tx_queue[idx * 0xC];

        if (slot[1] != 0) {
            slot[1] = (uint8_t)(slot[1] - 1);
        }

        if (g_ssp_ble_tx_queue[idx * 0xC + 1] == 0) {
            /* Retry budget exhausted: report, free payload, drop slot. */
            g_log_func("BLE remove id %X nr %X\r\n",
                       *(uint16_t *)&slot[4], slot[3]);
            free(*(void **)&g_ssp_ble_tx_queue[g_ssp_ble_tx_queue[0x602] * 0xC + 8]);
            g_ssp_ble_tx_queue[0x601]++;
            return 2;
        }

        /* Build the SLIP frame in the +0x604 staging buffer. */
        {
            uint8_t *frame = &g_ssp_ble_tx_queue[0x604];
            int len;

            frame[0] = 1;
            if (slot[0] == 1) {                 /* short frame */
                frame[1] = 6;
                frame[2] = slot[3];
                {
                    uint16_t v = *(uint16_t *)&slot[4];
                    frame[3] = (uint8_t)v;
                    frame[4] = (uint8_t)(v >> 8);
                }
                len = 5;
            } else {
                if (slot[0] != 0) {
                    muco_assert_fail("src/ssp_ble.c", 200);
                }
                frame[1] = 7;
                frame[2] = slot[3];
                {
                    uint16_t v = *(uint16_t *)&slot[4];
                    frame[3] = (uint8_t)v;
                    frame[4] = (uint8_t)(v >> 8);
                }
                {
                    uint16_t plen = *(uint16_t *)&slot[6];
                    frame[5] = (uint8_t)plen;
                    frame[6] = (uint8_t)(plen >> 8);
                    memcpy(&frame[7], *(void **)&slot[8], plen);
                    len = plen + 7;
                }
            }
            /* OEM loads 0x20009044 (== &queue[0x604]) as an address immediate and
             * passes it directly — there is no separate pointer variable there. */
            slip_send_frame(&g_ssp_ble_tx_queue[0x604], len);
            return 0;
        }
    }
}

/* ── inter-module (SSPM) bus TX path ─────────────────────────────────────────
 * The mainware reaches the battery/motor/shifter modules over a second serial
 * link (distinct from the BLE-coprocessor UART used by uart_send_byte). This is
 * its byte primitive and SLIP framer; sspm_tx_queue_pump above feeds it. */

/* Push one byte into the SSPM-bus TX ring (OEM sspm_bus_send_byte, 0x0803662C).
 * The twin of uart_send_byte, but for the inter-module-bus peripheral: the
 * device register block is reached through the pointer at 0x20009924, and the
 * TX-ring handle lives at a fixed slot 0xA50 into the bus context at 0x20002B3C.
 * The control register at dev+0xC carries TX-interrupt-enable in bit 7; the OEM
 * masks it (with a DSB/ISB pair so the disable lands) around the ring push, then
 * re-enables it after re-loading the handle.
 *
 * Same ABI quirk as uart_send_byte: the function never recomputes a return value
 * — ringbuf_push_byte's status survives in r0 through the trailing register
 * re-enable, so it implicitly returns that status (1 = pushed, 0 = ring full).
 * sspm_bus_send_frame maps a 0 here to a TX error. */
int sspm_bus_send_byte(uint8_t b)
{
    volatile uint32_t *dev = *(volatile uint32_t * volatile *)0x20009924u;

    dev[0xC / 4] &= ~0x80u;                       /* mask TX interrupt (TXEIE) */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    ringbuf_t *rb = *(ringbuf_t * volatile *)(0x20002B3Cu + 0xA50u);
    uint32_t rc = ringbuf_push_byte(rb, b);

    dev = *(volatile uint32_t * volatile *)0x20009924u;   /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TX interrupt */
    return (int)rc;
}

/* SLIP byte emit with escaping (helper for sspm_bus_send_frame). Mirrors slip_put
 * but routes through the SSPM-bus byte primitive. */
static void sspm_put(uint8_t b)
{
    if (b == SLIP_END) {
        sspm_bus_send_byte(SLIP_ESC);
        sspm_bus_send_byte(SLIP_ESC_END);
    } else if (b == SLIP_ESC) {
        sspm_bus_send_byte(SLIP_ESC);
        sspm_bus_send_byte(SLIP_ESC_ESC);
    } else {
        sspm_bus_send_byte(b);
    }
}

/* SLIP-frame + transmit a buffer over the inter-module (SSPM) bus with a CRC-16
 * trailer (OEM sspm_bus_send_frame, 0x0803A008). The SSPM-bus counterpart of
 * slip_send_frame; wire format is identical —
 *   0xC0 [escaped payload] [escaped CRC-lo] [escaped CRC-hi] 0xC0
 * with the little-endian CRC-16 appended. Returns 0 on success, 2 if the closing
 * delimiter could not be queued (only that final byte's TX status is checked,
 * matching the OEM). */
uint32_t sspm_bus_send_frame(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = crc16(buf, len, 0xFFFFu);

    sspm_bus_send_byte(SLIP_END);
    while (len != 0) {
        sspm_put(*buf);
        buf++;
        len--;
    }
    sspm_put((uint8_t)(crc & 0xFF));
    sspm_put((uint8_t)((crc >> 8) & 0xFF));

    if (sspm_bus_send_byte(SLIP_END) == 0) {
        return 2;
    }
    return 0;
}

/* Pull one byte from the SSPM-bus RX ring (OEM sspm_bus_get_byte, 0x08036664).
 * The RX twin of sspm_bus_send_byte: the same device register block (pointer at
 * 0x20009924), but the RX-ring handle is at slot 0xA54 of the bus context at
 * 0x20002B3C, and the control register at dev+0xC has RX-interrupt-enable in bit
 * 5 (0x20) — masked (with a DSB/ISB pair) around the ring access, then restored.
 *
 * Same ABI quirk as ssp_rx_byte / sspm_bus_send_byte: the function leaves
 * ringbuf_get_byte's status in r0 untouched through the trailing register
 * re-enable, so it implicitly returns that status (1 = byte produced, 0 = empty).
 * The motor-DSP download pumps and the SSPM RX de-framer rely on this. */
int sspm_bus_get_byte(uint8_t *out)
{
    volatile uint32_t *dev = *(volatile uint32_t * volatile *)0x20009924u;

    dev[0xC / 4] &= ~0x20u;                       /* mask RX interrupt (RXNEIE) */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    ringbuf_t *rb = *(ringbuf_t * volatile *)(0x20002B3Cu + 0xA54u);
    uint32_t rc = ringbuf_get_byte(rb, out);

    dev = *(volatile uint32_t * volatile *)0x20009924u;   /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RX interrupt */
    return (int)rc;
}
