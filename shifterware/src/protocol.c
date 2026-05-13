/* protocol.c — framed UART link to the main module. */

#include "protocol.h"
#include "uart.h"

typedef enum {
    RX_WAIT_SOF = 0,
    RX_ID,
    RX_LEN,
    RX_PAYLOAD,
    RX_CRC,
} rx_state_t;

static rx_state_t   s_state;
static proto_frame_t s_partial;
static uint8_t      s_payload_idx;

static proto_frame_t s_ready;
static bool          s_ready_full;

void protocol_init(void)
{
    s_state       = RX_WAIT_SOF;
    s_payload_idx = 0u;
    s_ready_full  = false;
    uart1_init(115200u);
}

uint8_t proto_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0u; b < 8u; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool protocol_send(uint8_t id, const uint8_t *payload, uint8_t len)
{
    if (len > PROTO_MAX_PAYLOAD) return false;

    uint8_t hdr[2 + PROTO_MAX_PAYLOAD];
    hdr[0] = id;
    hdr[1] = len;
    for (uint8_t i = 0u; i < len; i++) {
        hdr[2u + i] = payload[i];
    }
    const uint8_t crc = proto_crc8(hdr, (size_t)(2u + len));

    uart1_send_byte(PROTO_SOF);
    uart1_send(hdr, (size_t)(2u + len));
    uart1_send_byte(crc);
    return true;
}

static void feed_byte(uint8_t b)
{
    switch (s_state) {
    case RX_WAIT_SOF:
        if (b == PROTO_SOF) s_state = RX_ID;
        break;

    case RX_ID:
        s_partial.id = b;
        s_state = RX_LEN;
        break;

    case RX_LEN:
        if (b > PROTO_MAX_PAYLOAD) {
            s_state = RX_WAIT_SOF;
            break;
        }
        s_partial.len = b;
        s_payload_idx = 0u;
        s_state = (b == 0u) ? RX_CRC : RX_PAYLOAD;
        break;

    case RX_PAYLOAD:
        s_partial.payload[s_payload_idx++] = b;
        if (s_payload_idx >= s_partial.len) {
            s_state = RX_CRC;
        }
        break;

    case RX_CRC: {
        uint8_t hdr[2 + PROTO_MAX_PAYLOAD];
        hdr[0] = s_partial.id;
        hdr[1] = s_partial.len;
        for (uint8_t i = 0u; i < s_partial.len; i++) {
            hdr[2u + i] = s_partial.payload[i];
        }
        const uint8_t expect = proto_crc8(hdr, (size_t)(2u + s_partial.len));
        if (expect == b && !s_ready_full) {
            s_ready      = s_partial;
            s_ready_full = true;
        }
        s_state = RX_WAIT_SOF;
        break;
    }
    }
}

void protocol_poll(void)
{
    while (uart1_rx_available()) {
        feed_byte(uart1_rx_byte());
    }
}

bool protocol_recv(proto_frame_t *out)
{
    if (!s_ready_full) return false;
    *out = s_ready;
    s_ready_full = false;
    return true;
}
