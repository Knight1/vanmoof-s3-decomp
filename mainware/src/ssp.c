#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crc.h"
#include "log.h"
#include "panic.h"
#include "ssp.h"
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
