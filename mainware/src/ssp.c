#include <stdint.h>

#include "log.h"
#include "ssp.h"

/* --- externs supplied elsewhere in the image -------------------------------
 * ssp_rx_byte       0x08036528  pop one byte from the bus RX ring (atomic).
 * crc16             0x0803C2C8  Modbus-bus CRC-16 over (buf,len) with init.
 * ble_cmd_dispatch  0x08033970  command-id dispatcher (lock/region/power/...).
 * The three message-type handlers are thin shims onto the dispatcher / control
 * paths; kept opaque here (the dispatcher itself is mapped, not sourced). */
extern int  ssp_rx_byte(uint8_t *out);
extern int  crc16(const uint8_t *buf, int len, int init);
extern void ble_data_packet(uint16_t cmd, uint16_t len, uint8_t *data, uint8_t len_hi); /* 0x0803F6A4 */
extern void ble_prepare_packet(uint16_t len);                                            /* 0x0803F6AC */
extern int  ble_command(uint8_t id);                                                     /* 0x0803F498 */
extern void ble_send_response(uint8_t *resp, int len);                                   /* 0x0803F4F0 */

/* The BLE/SSP receive context (OEM SRAM 0x20008A40). Only the fields the
 * transport touches are modelled; `slip_state` is the de-framer state byte. */
struct ble_ssp_ctx {
    uint8_t _pad0[0x600];
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
            ble_send_response(g_ble_ssp.response, 3);
        } else if (type == BLE_MSG_PREPARE) {        /* 0x06 — length announce */
            ble_prepare_packet((uint16_t)(msg[3] | (msg[4] << 8)));
            g_ble_ssp.response[0] = 1;
            g_ble_ssp.response[1] = 5;
            g_ble_ssp.response[2] = id;
            ble_send_response(g_ble_ssp.response, 3);
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
